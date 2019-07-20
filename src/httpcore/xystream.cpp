#include "xystream.h"
#include <cstring>
#include <iostream>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>

const char *int_status::strerror() {
    return uv_strerror(_status);
}

int_status::~int_status() {}

write_request::write_request(const string &data) : _cur(0) {
    _data = (char *)malloc(data.size());
    if(_data == NULL)
        throw runtime_error("no enough memory");
    _len = data.size();
    memcpy(_data, data.data(), _len);
}

write_request::write_request(char *data, size_t len)
: _data(data), _len(len), _cur(0) {}

write_request::write_request(shared_ptr<message> data) : _cur(0) {
    _len = data->serialize_size();
    _data = (char *)malloc(_len);
    if(_data == NULL)
        throw runtime_error("cannot allocate enough memory");
    data->serialize(_data);
}

void write_request::associate(shared_ptr<fiber> f) {
    _bound_fiber = f;
}

bool write_request::confirm(int len) {
    int newCur = _cur + len;
    if(newCur > _len)
        throw RTERR("confirmation to bytes out of write request");
    _cur = newCur;
    if(_cur == _len) {
        if(_bound_fiber)
            _bound_fiber->resume(int_status::make(len));
        return true;
    } else {
        return false;
    }
}

void write_request::abort(int eno) {
    _bound_fiber->raise(uv_strerror(uv_translate_sys_error(eno)));
}

write_request::~write_request() {
    free(_data);
}

static void stream_event_handler(uv_poll_t* handle, int status, int events)
{
    stream *self = (stream *)handle->data;
    self->dispatch_event(events);
}

stream::stream(int fd) : _fd(fd), buffer(new streambuffer()), _connecting(false) {
    int r;
    if((r = uv_poll_init(uv_default_loop(), &_poller, _fd)) < 0)
        throw IOERR(r);
    _poller.data = this;
    if((r = uv_poll_start(&_poller, UV_READABLE | UV_WRITABLE | UV_DISCONNECT,
                          stream_event_handler)) < 0)
        throw IOERR(r);
}

int stream::fill_read_buffer(shared_ptr<decoder> tryDecoder) {
    while(true) {
        char *buf = buffer->prepare(READ_STEP);
        if(!buf) break; // Memory not enough, give up reading
        int nread = recv(_fd, buf, READ_STEP, 0);
        if(nread > 0) {
            buffer->enlarge(nread);
            if(tryDecoder && tryDecoder->decode(buffer))
                return 0;
        } else {
            _readable = false;
            if(nread == 0) {
                _eof = true;
                break;
            }
            else if(errno == EWOULDBLOCK || errno == EAGAIN)
                break;
            return errno;
        }
    }
    return EAGAIN;
}

void stream::dispatch_event(int events) {
    if(events & UV_WRITABLE) {
        if(_connecting) {
            int err;
            socklen_t len = sizeof(err);
            _connecting = false;
            getsockopt(_fd, SOL_SOCKET, SO_ERROR, &err, &len);
            _reading_fiber->resume(int_status::make(err));
        } else {
            flush_write_queue();
        }
    }
    if(events & UV_READABLE) {
        _readable = true;
        if(_reading_fiber) {
            try {
                int r = fill_read_buffer(_reading_decoder);
                if(r != EAGAIN) {
                    _reading_decoder.reset();
                    _reading_fiber->resume(int_status::make(r));
                }
            }
            catch(runtime_error &ex) {
                _reading_decoder.reset();
                _reading_fiber->raise(string("Protocol error: ") + ex.what());
            }
        }
    }
}

void stream::flush_write_queue() {
    while(!_write_queue.empty()) {
        auto req = _write_queue.front();
        int nwrite = ::write(_fd, req->base(), req->size());
        if(nwrite < 0) {
            if(errno == EWOULDBLOCK || errno == EAGAIN)
                break;
            _write_queue.pop();
            req->abort(errno);
            continue;
        }
        if(req->confirm(nwrite))
            _write_queue.pop();
    }
}

shared_ptr<message> stream::read(shared_ptr<decoder> decoder) {
    if(_reading_fiber)
        throw RTERR("reading from a stream occupied by another fiber");
    if(buffer->size() > 0) // First check whether streambuffer has data
        if(decoder->decode(buffer))
            return decoder->msg();
    if(_readable) { // Second check whether OS socket buffer has data
        int r = fill_read_buffer(decoder);
        if(r == 0)
            return decoder->msg();
        else if(r == EOF)
            return nullptr;
        else if(r != EAGAIN)
            throw IOERR(r);
    }
    if(!fiber::in_fiber())
        throw logic_error("reading from a stream outside a fiber");
    _reading_decoder = decoder;
    _reading_fiber = fiber::current();
    int s;
    try { // Otherwise wait data
        s = fiber::yield<int_status>()->status();
    }
    catch(runtime_error &ex) {
        throw RTERR("Protocol error: %s", ex.what());
    }
    _reading_fiber.reset();
    if(s == 0)
        return decoder->msg();
    else if(s == EOF)
        return nullptr;
    else
        throw IOERR(s);
}

void stream::write(const char *chunk, int length)
{
    char *buf = (char *)malloc(length);
    if(!buf)
        throw RTERR("memory allocation failed");
    memcpy(buf, chunk, length);
    do_write_request(make_shared<write_request>(buf, length));
}

void stream::write(shared_ptr<message> msg) {
    do_write_request(make_shared<write_request>(msg));
}

void stream::write(const string &str) {
    do_write_request(make_shared<write_request>(str));
}

void stream::do_write_request(shared_ptr<write_request> req) {
    req->associate(fiber::current());
    _write_queue.push(req);
    flush_write_queue();
    fiber::yield<int_status>();
}

bool stream::has_tls() {
    return false;
}

void stream::connect(const sockaddr *addr, int len) {
    if(::connect(_fd, addr, len) == 0)
        return;
    if(errno == EINPROGRESS) {
        _connecting = true;
        _reading_fiber = fiber::current();
        auto s = fiber::yield<int_status>();
        _reading_fiber.reset();
        if(s->status() != 0)
            throw IOERR(uv_translate_sys_error(s->status()));
    }
}

stream::~stream() {
    // TODO: fix resource release on next uv_loop_t iteration
    uv_close((uv_handle_t *)&_poller, nullptr);
    close(_fd);
}

tcp_stream::tcp_stream() : stream(socket(AF_INET, SOCK_STREAM, 0)) {
    if(_fd < 0)
        throw RTERR("failed while creating socket");
}

tcp_stream::tcp_stream(int fd, shared_ptr<ip_endpoint> ep)
: stream(fd), _remote_ep(ep) {}

void tcp_stream::connect(const string &host, int port)
{
    connect(make_shared<ip_endpoint>(host, port));
}

void tcp_stream::connect(shared_ptr<ip_endpoint> ep) {
    _remote_ep = ep;
    stream::connect(ep->sa(), ep->size());
}

shared_ptr<ip_endpoint> tcp_stream::getpeername() {
    return _remote_ep;
}

shared_ptr<tcp_stream> tcp_stream::accept(int fd) {
    sockaddr_storage sa;
    socklen_t len = sizeof(sa);
    int newFd = ::accept(fd, (sockaddr *)&sa, &len);
    if(newFd < 0)
        throw IOERR(errno);
    return shared_ptr<tcp_stream>(new tcp_stream(
            newFd, make_shared<ip_endpoint>(&sa)));
}

unix_stream::unix_stream() : stream(socket(AF_UNIX, SOCK_STREAM, 0)) {}

void unix_stream::connect(const shared_ptr<string> path)
{
    sockaddr_un addr;
    addr.sun_family = AF_UNIX;
    strcpy(addr.sun_path, path->c_str());
    stream::connect((sockaddr *)&addr, sizeof(addr));
}

ip_endpoint::ip_endpoint(struct sockaddr_storage *sa) {
    if (sa->ss_family == AF_INET)
        memcpy(&_sa, sa, sizeof(struct sockaddr_in));
    else if (sa->ss_family == AF_INET6)
        memcpy(&_sa, sa, sizeof(struct sockaddr_in6));
}

ip_endpoint::ip_endpoint(const char *addr, int p) {
    if(uv_ip4_addr(addr, p, &_sa_in) && uv_ip6_addr(addr, p, &_sa_in6))
        throw RTERR("invalid IP address or port");
}

ip_endpoint::ip_endpoint(const string &addr, int p)
    : ip_endpoint(addr.c_str(), p) {}

int ip_endpoint::port() {
    if (_sa.ss_family == AF_INET)
        return ntohs(_sa_in.sin_port);
    else if (_sa.ss_family == AF_INET6)
        return ntohs(_sa_in6.sin6_port);
}

const sockaddr *ip_endpoint::sa() {
    return (sockaddr *)&_sa;
}

const size_t ip_endpoint::size() {
    switch(_sa.ss_family) {
        case AF_INET:
            return sizeof(_sa_in);
        case AF_INET6:
            return sizeof(_sa_in6);
        default:
            return 0;
    }
}

shared_ptr<string> ip_endpoint::straddr() {
    char ip[20];
    if (_sa.ss_family == AF_INET)
        uv_inet_ntop(AF_INET, &_sa_in.sin_addr, ip, sizeof(ip));
    else if (_sa.ss_family == AF_INET6)
        uv_inet_ntop(AF_INET6, &_sa_in6.sin6_addr, ip, sizeof(ip));
    return shared_ptr<string>(new string(ip));
}

string_message::string_message(const string &s)
        : _str(s) {}


string_message::string_message(const char *buf, int len)
        : _str(buf, len) {}

int string_message::type() const {
    return XY_MESSAGE_STRING;
}

int string_message::serialize_size() {
    return _str.size();
}

void string_message::serialize(char *buf) {
    memcpy(buf, _str.data(), _str.size());
}

string_message::~string_message() {}

string_decoder::string_decoder() : nbyte(0), buffer(nullptr) {}

shared_ptr<message> string_decoder::msg() {
    if(buffer)
        return shared_ptr<string_message>(
            new string_message(buffer, nbyte));
    return nullptr;
}

bool string_decoder::decode(shared_ptr<streambuffer> &stb) {
    if(stb->size() > 0) {
        if(buffer) free(buffer);
        nbyte = stb->size();
        buffer = stb->detach();
        return true;
    }
    return false;
}

string_decoder::~string_decoder() {
    if(buffer) free(buffer);
}

rest_decoder::rest_decoder(int rest) :
    nbyte(0), nrest(rest), buffer(nullptr) {}

shared_ptr<message> rest_decoder::msg() {
    if(buffer) {
        return make_shared<string_message>(buffer, nbyte);
    }
}

bool rest_decoder::decode(shared_ptr<streambuffer> &stb) {
    if(nrest == 0)
        throw runtime_error("no more data to read");
    if(buffer) free(buffer);
    if(stb->size() <= nrest) {
        nbyte = stb->size();
        buffer = stb->detach();
        nrest -= nbyte;
    } else {
        nbyte = nrest;
        buffer = (char *)malloc(nbyte);
        memcpy(buffer, stb->data(), nbyte);
        stb->pull(nbyte);
        nrest -= nbyte;
    }
    return true;
}

rest_decoder::~rest_decoder() {
    if(buffer) free(buffer);
}