#include "xystream.h"
#include <cstring>
#include <iostream>

using namespace std;

chunk::chunk(const char *buf, size_t siz) : _X(nullptr) {
    if(buf == nullptr)
        return;
    _X = (_storage *)malloc(sizeof(_storage) + siz + 1);
    if(!_X)
        throw std::bad_alloc();
    _X->_nRef = 1;
    _X->_size = siz;
    memcpy(_X->Y, buf, siz);
    _X->Y[siz] = 0;
}

void message::serialize(char *buf) {
    throw RTERR("serialize not implemented");
}

message::~message() {}

stream_buffer::stream_buffer() : _data(NULL), _avail(0) {}

/**
 * Remove n bytes of proceeded data remove the beginning of buffer.
 * @param nbytes Byte count to remove.
 */
void stream_buffer::pull(int nbytes) {
    if(nbytes < _avail) {
        memmove(_data, _data + nbytes, _avail - nbytes);
        _avail -= nbytes;
        _data = (char *)realloc(_data, _avail);
    } else {
        _avail = 0;
        free(_data);
        _data = NULL;
    }
}

/**
 * Commit n bytes of new data at the prepared area of buffer.
 * @param nbytes Byte count to commit.
 */
void stream_buffer::commit(size_t nbytes) {
    _avail += nbytes;
    // Shrink the buffer. If returned address is not _data, logic
    // error or memory overrun must occurred. Check it!
    _data = (char *)realloc(_data, _avail);
}

/**
 * Serial message in msg and append the serialized data to the buffer.
 * @param msg Pointer to the message.
 */
void stream_buffer::append(const P<message> &msg) {
    int size = msg->serialize_size();
    if(size > 0) {
        msg->serialize(prepare(size));
        commit(size);
    }
}

/**
 * Append n bytes of data at address p to the end of buffer.
 * @param data Address to new data.
 * @param nbytes Byte count.
 */
void stream_buffer::append(const void *p, int nbytes) {
    memcpy(prepare(nbytes), p, nbytes);
    commit(nbytes);
}

/**
 * Preallocate n bytes of memory at the end of buffer.
 * If there was pre-allocated area, previous data is preserved.
 * @param nbytes Byte count to preallocate.
 * @return The address to the beginning of pre-allocated area.
 */
char *stream_buffer::prepare(size_t nbytes) {
    char *p = (char *)realloc(_data, _avail + nbytes);
    if(!p) throw std::bad_alloc();
    _data = p;
    return _data + _avail;
}

/**
 * Detach all data in buffer. Ownership of the buffer is transferred,
 * thus you should free the returned buffer later.
 * @return The beginning address of the buffer.
 */
char *stream_buffer::detach() {
    char *_old = _data;
    _data = nullptr;
    _avail = 0;
    return _old;
}

stream_buffer::~stream_buffer() {
    free(_data);
    _avail = 0;
}

decoder::~decoder() {}

string_message::string_message(const char *buf, size_t len) : _data(buf, len) {}

int string_message::serialize_size() {
    return _data.size();
}

void string_message::serialize(char *buf) {
    memcpy(buf, _data.data(), _data.size());
}

string_message::~string_message() = default;

string_decoder::string_decoder(int bytesToRead)
        : _nBytes(0), _restBytes(bytesToRead) {}

bool string_decoder::decode(stream_buffer &stb) {
    if(stb.size() == 0)
        return false;
    _nBytes = stb.size();
    if(_restBytes == -1) {
        _msg = make_shared<string_message>(stb.data(), _nBytes);
        stb.pull(_nBytes);
        return true;
    }
    if(_nBytes > _restBytes)
        _nBytes = _restBytes;
    _msg = make_shared<string_message>(stb.data(), _nBytes);
    stb.pull(_nBytes);
    _restBytes -= _nBytes;
    return true;
}

string_decoder::~string_decoder() {}

stream::stream() : _timeout(15000) {
    _timeOuter = mem_alloc<uv_timer_t>();
    if(uv_timer_init(uv_default_loop(), _timeOuter) < 0) {
        free(_timeOuter);
        throw runtime_error("failed to setup timeout timer");
    }
    _timeOuter->data = this;
}

stream::~stream() {
    if(handle)
        uv_close((uv_handle_t *)handle, (uv_close_cb) free);
    uv_close((uv_handle_t *)_timeOuter, (uv_close_cb) free);
}

void stream::accept(uv_stream_t *svr) {
    int r = uv_accept(svr, handle);
    if(r < 0)
        throw IOERR(r);
}

stream::write_request::write_request() {
    write_req.data = this;
}

class stream::callbacks {
public:
    static void on_alloc(uv_handle_t* handle, size_t suggested_size, uv_buf_t* buf) {
        stream *self = (stream *)handle->data;
        buf->base = self->buffer.prepare(suggested_size);
        buf->len = buf->base ? suggested_size : 0;
    }

    static void on_read_timeout(uv_timer_t* handle) {
        stream *self = (stream *)handle->data;
        uv_timer_stop(handle);
        self->read_cont.resume(UV_ETIMEDOUT);
    }

    static void on_data(uv_stream_t* handle, ssize_t nread, const uv_buf_t* buf) {
        stream *self = (stream *)handle->data;
        if(nread != 0) {
            uv_timer_stop(self->_timeOuter);
            self->_commit_rx(buf->base, nread);
        }
    }
};

void stream::read(const shared_ptr<decoder> &decoder) {
    if(read_cont)
        throw RTERR("stream is read-busy");
    if(buffer.size() > 0 && decoder->decode(buffer))
        return;
    _decoder = decoder;
    int status = _do_read();
    _decoder.reset();
    if(status < 0)
        throw IOERR(status);
}

static void stream_on_write(uv_write_t *req, int status)
{
    auto *self = (stream::write_request *)req->data;
    self->_cont.resume(status);
    delete self;
}

void stream::write(const char *chunk, int length)
{
    uv_buf_t buf;
    buf.base = (char *)chunk;
    buf.len = length;
    auto *wreq = new write_request;
    int r = uv_write(&wreq->write_req, handle, &buf, 1, stream_on_write);
    if(r < 0) {
        delete wreq;
        throw IOERR(r);
    }
    int status = fiber::yield(wreq->_cont);
    if(status != 0)
        throw IOERR(status);
}

void stream::write(const shared_ptr<message> &msg) {
    int size = msg->serialize_size();
    char *buf = new char[size];
    msg->serialize(buf);
    write(buf, size);
    delete[] buf;
}

void stream::write(const chunk &str) {
    write(str.data(), str.size());
}

void stream::_commit_rx(char *base, int nread) {
    if(nread < 0) {
        read_cont.resume(nread);
        return;
    }
    buffer.commit(nread);
    try {
        if(_decoder->decode(buffer)) {
            read_cont.resume(nread);
            return;
        }
        if(_timeout > 0)
            uv_timer_start(_timeOuter, callbacks::on_read_timeout, _timeout, 0);
    }
    catch(runtime_error &ex) {
        _decoder.reset();
        read_cont.resume(UV_EINVAL);
    }
}

int stream::_do_read() {
    int r;
    if((r = uv_read_start(handle, callbacks::on_alloc, callbacks::on_data)) < 0)
        throw IOERR(r);
    if(_timeout > 0)
        uv_timer_start(_timeOuter, callbacks::on_read_timeout, _timeout, 0);
    int status = fiber::yield(read_cont);
    uv_read_stop(handle);
    return status;
}

void stream::set_timeout(int timeout) {
    _timeout = timeout;
}

static void stream_on_shutdown(uv_shutdown_t* req, int status) {
    auto self = (stream::write_request *)req->data;
    self->_cont.resume(status);
    delete self;
}

void stream::shutdown() {
    auto *req = new write_request;
    int r = uv_shutdown(&req->shutdown_req, handle, stream_on_shutdown);
    if(r < 0) {
        delete req;
        throw IOERR(r);
    }
    fiber::yield(req->_cont);
}

bool stream::has_tls() {
    return false;
}

tcp_stream::tcp_stream() {
    uv_tcp_t *h = mem_alloc<uv_tcp_t>();
    if(uv_tcp_init(uv_default_loop(), h) < 0) {
        free(h);
        throw runtime_error("failed to initialize libuv TCP stream");
    }
    handle = (uv_stream_t *)h;
    handle->data = this;
}

static void stream_on_connect(uv_connect_t* req, int status) {
    auto self = (stream::write_request *)req->data;
    self->_cont.resume(status);
    delete self;
}

void tcp_stream::connect(const string &host, int port)
{
    ip_endpoint ep(host, port);
    connect(ep.sa());
}

void tcp_stream::connect(shared_ptr<ip_endpoint> ep) {
    connect(ep->sa());
}

void tcp_stream::connect(const sockaddr *sa) {
    auto *req = new write_request;
    int r = uv_tcp_connect(&req->connect_req, (uv_tcp_t *)handle, sa, stream_on_connect);
    if(r < 0) {
        delete req;
        throw IOERR(r);
    }
    int status = fiber::yield(req->_cont);
    if(status != 0)
        throw IOERR(status);
}

void tcp_stream::nodelay(bool enable) {
    int r = uv_tcp_nodelay((uv_tcp_t *)handle, enable);
    if(r < 0) throw IOERR(r);
}

shared_ptr<ip_endpoint> tcp_stream::getpeername() {
    struct sockaddr_storage address;
    int addrlen = sizeof(address);
    int r = uv_tcp_getpeername(
        (uv_tcp_t *)handle, (struct sockaddr*)&address, &addrlen);
    if(r < 0) throw IOERR(r);
    return make_shared<ip_endpoint>(&address);
}

unix_stream::unix_stream() {
    uv_pipe_t *h = mem_alloc<uv_pipe_t>();
    if(uv_pipe_init(uv_default_loop(), h, 0) < 0) {
        free(h);
        throw runtime_error("failed to initialize libuv UNIX stream");
    }
    handle = (uv_stream_t *)h;
    handle->data = this;
}

void unix_stream::connect(const string &path)
{
    auto *req = new write_request;
    uv_pipe_connect(&req->connect_req, (uv_pipe_t *)handle, path.c_str(), stream_on_connect);
    int status = fiber::yield(req->_cont);
    if(status != 0)
        throw IOERR(status);
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

string ip_endpoint::straddr() {
    char ip[20];
    if (_sa.ss_family == AF_INET)
        uv_inet_ntop(AF_INET, &_sa_in.sin_addr, ip, sizeof(ip));
    else if (_sa.ss_family == AF_INET6)
        uv_inet_ntop(AF_INET6, &_sa_in6.sin6_addr, ip, sizeof(ip));
    return string(ip);
}

tcp_server::tcp_server(const char *addr, int port) {
    ip_endpoint ep(addr, port);
    _server = (uv_tcp_t *)malloc(sizeof(uv_tcp_t));
    if(uv_tcp_init(uv_default_loop(), _server) < 0) {
        free(_server);
        throw runtime_error("failed to initialize libuv TCP stream");
    }
    _server->data = this;
    int r = uv_tcp_bind(_server, ep.sa(), 0);
    if(r < 0) {
        uv_close((uv_handle_t *)_server, (uv_close_cb)free);
        throw runtime_error(uv_strerror(r));
    }
}

tcp_server::~tcp_server() {
    uv_close((uv_handle_t *)_server, (uv_close_cb) free);
}

static void tcp_server_on_connection(uv_stream_t* strm, int status) {
    tcp_server *self = (tcp_server *)strm->data;
    if(status >= 0) {
        auto client = make_shared<tcp_stream>();
        try {
            client->accept(strm);
            client->nodelay(true);
            fiber::launch(bind(self->service_func, client));
        }
        catch(exception &ex) {}
    }
}

void tcp_server::serve(function<void(shared_ptr<tcp_stream>)> f) {
    service_func = f;
    int r = uv_listen((uv_stream_t *)_server, 32, tcp_server_on_connection);
    if(r < 0)
        throw runtime_error(uv_strerror(r));
}