#include "xystream.h"
#include <cstring>
#include <iostream>

const char *int_status::strerror() {
    return uv_strerror(_status);
}

int_status::~int_status() {}

stream::stream() : buffer(new streambuffer()) {
    _wreq.data = this;
}

void stream::accept(uv_stream_t *svr) {
    int r = uv_accept(svr, handle);
    if(r < 0)
        throw IOERR(r);
}

static void stream_on_alloc(uv_handle_t* handle, size_t suggested_size, uv_buf_t* buf) {
    stream *self = (stream *)handle->data;
    buf->base = (char *)self->buffer->prepare(suggested_size);
    buf->len = buf->base ? suggested_size : 0;
}

static void stream_on_data(uv_stream_t* handle, ssize_t nread, const uv_buf_t* buf) {
    stream *self = (stream *)handle->data;
    if(nread > 0) self->buffer->enlarge(nread);
    self->reading_fiber->resume(int_status::make(nread));
}

shared_ptr<message> stream::read(shared_ptr<decoder> decoder) {
    if(reading_fiber)
        throw RTERR("reading from a stream occupied by another fiber");
    if(buffer->size() > 0)
        if(decoder->decode(buffer))
            return decoder->msg();
    if(!fiber::in_fiber())
        throw logic_error("reading from a stream outside a fiber");
    int r;
    if((r = uv_read_start(handle, stream_on_alloc, stream_on_data)) < 0)
        throw IOERR(r);
    reading_fiber = fiber::running();
    while(true) {
        //TODO: let stream_on_data directly invoke decoder
        //to reduce context switches
        auto s = fiber::yield<int_status>();
        if(s->status() >= 0) {
            try {
                if(decoder->decode(buffer)) {
                    uv_read_stop(handle);
                    reading_fiber.reset();
                    return decoder->msg();
                }
            }
            catch(exception &ex) {
                uv_read_stop(handle);
                reading_fiber.reset();
                throw RTERR("Protocol Error: %s", ex.what());
            }
        }
        else {
            uv_read_stop(handle);
            reading_fiber.reset();
            if(s->status() == UV_EOF)
                return nullptr;
            throw IOERR(s->status());
        }
    }
}

static void stream_on_write(uv_write_t *req, int status)
{
    stream *self = (stream *)req->data;
    self->writing_fiber->resume(int_status::make(status));
}

void stream::write(const char *chunk, int length)
{
    if(writing_fiber)
        throw runtime_error("writing to a stream occupied by another fiber");
    int r;
    uv_buf_t buf;
    buf.base = (char *)chunk;
    buf.len = length;
    if((r = uv_write(&_wreq, handle, &buf, 1, stream_on_write)) < 0)
        throw IOERR(r);
    writing_fiber = fiber::running();
    auto s = fiber::yield<int_status>();
    writing_fiber.reset();
    if(s->status() != 0)
        throw IOERR(s->status());
}

void stream::write(shared_ptr<message> msg) {
    int size = msg->serialize_size();
    char *buf = new char[size];
    msg->serialize(buf);
    write(buf, size);
    delete[] buf;
}

void stream::write(const string &str) {
    write(str.data(), str.size());
}

bool stream::has_tls() {
    return false;
}

stream::~stream() {
    if(handle) {
        uv_close((uv_handle_t *)handle, (uv_close_cb) free);
    }
}

tcp_stream::tcp_stream() {
    uv_tcp_t *h = (uv_tcp_t *)malloc(sizeof(uv_tcp_t));
    if(uv_tcp_init(uv_default_loop(), h) < 0) {
        free(h);
        throw runtime_error("failed to initialize libuv TCP stream");
    }
    handle = (uv_stream_t *)h;
    handle->data = this;
}

static void tcp_on_connect(uv_connect_t* req, int status) {
    stream *self = (stream *)req->data;
    delete req;
    self->writing_fiber->event = int_status::make(status);
    self->writing_fiber->resume();
}

void tcp_stream::connect(const string &host, int port)
{
    ip_endpoint ep(host, port);
    uv_connect_t *req = new uv_connect_t;
    req->data = this;
    int r = uv_tcp_connect(req, (uv_tcp_t *)handle, ep.sa(), tcp_on_connect);
    if(r < 0) {
        throw IOERR(r);
        delete req;
    }
    writing_fiber = fiber::running();
    auto s = fiber::yield<int_status>();
    writing_fiber.reset();
    if(s->status() != 0)
        throw IOERR(s->status());
}

shared_ptr<ip_endpoint> tcp_stream::getpeername() {
    struct sockaddr_storage address;
    int addrlen = sizeof(address);
    int r = uv_tcp_getpeername(
        (uv_tcp_t *)handle, (struct sockaddr*)&address, &addrlen);
    if(r < 0) throw IOERR(r);
    return shared_ptr<ip_endpoint>(new ip_endpoint(&address));
}

unix_stream::unix_stream() {
    uv_pipe_t *h = (uv_pipe_t *)malloc(sizeof(uv_pipe_t));
    if(uv_pipe_init(uv_default_loop(), h, 0) < 0) {
        free(h);
        throw runtime_error("failed to initialize libuv UNIX stream");
    }
    handle = (uv_stream_t *)h;
    handle->data = this;
}

static void pipe_on_connect(uv_connect_t* req, int status) {
    stream *self = (stream *)req->data;
    delete req;
    self->writing_fiber->event = int_status::make(status);
    self->writing_fiber->resume();
}

void unix_stream::connect(const shared_ptr<string> path)
{
    uv_connect_t *req = new uv_connect_t;
    req->data = this;
    uv_pipe_connect(req, (uv_pipe_t *)handle, path->c_str(), pipe_on_connect);
    writing_fiber = fiber::running();
    auto s = fiber::yield<int_status>();
    writing_fiber.reset();
    if(s->status() != 0)
        throw IOERR(s->status());
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
}

bool string_decoder::decode(shared_ptr<streambuffer> &stb) {
    if(stb->size() > 0) {
        if(buffer) delete buffer;
        nbyte = stb->size();
        buffer = new char[nbyte];
        memcpy(buffer, stb->data(), nbyte);
        stb->pull(nbyte);
        return true;
    }
    return false;
}

string_decoder::~string_decoder() {
    if(buffer) delete buffer;
}

rest_decoder::rest_decoder(int rest) :
    nbyte(0), nrest(rest), buffer(nullptr) {}

shared_ptr<message> rest_decoder::msg() {
    if(buffer) {
        shared_ptr<string_message> msg(
            new string_message(buffer, nbyte));
        delete buffer;
        buffer = nullptr;
        nbyte = 0;
        return msg;
    }
}

bool rest_decoder::decode(shared_ptr<streambuffer> &stb) {
    if(buffer) 
        throw runtime_error("take message first");
    if(nrest == 0)
        throw runtime_error("no more data to read");
    if(stb->size() <= nrest) {
        nbyte = stb->size();
    } else {
        nbyte = nrest;
    }
    buffer = new char[nbyte];
    memcpy(buffer, stb->data(), nbyte);
    stb->pull(nbyte);
    nrest -= nbyte;
    return true;
}

rest_decoder::~rest_decoder() {
    if(buffer) delete buffer;
}