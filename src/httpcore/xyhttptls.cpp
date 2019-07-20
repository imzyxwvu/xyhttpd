#include <iostream>
#include <openssl/err.h>
#include "xyhttptls.h"

static const char *sslerror_to_string(int err_code)
{
    switch (err_code)
    {
        case SSL_ERROR_NONE: return "SSL_ERROR_NONE";
        case SSL_ERROR_ZERO_RETURN: return "SSL_ERROR_ZERO_RETURN";
        case SSL_ERROR_SSL: return ERR_reason_error_string(ERR_get_error());
        case SSL_ERROR_WANT_READ: return "SSL_ERROR_WANT_READ";
        case SSL_ERROR_WANT_WRITE: return "SSL_ERROR_WANT_WRITE";
        case SSL_ERROR_WANT_X509_LOOKUP: return "SSL_ERROR_WANT_X509_LOOKUP";
        case SSL_ERROR_SYSCALL: return "SSL_ERROR_SYSCALL";
        case SSL_ERROR_WANT_CONNECT: return "SSL_ERROR_WANT_CONNECT";
        case SSL_ERROR_WANT_ACCEPT: return "SSL_ERROR_WANT_ACCEPT";
        default: return "SSL error";
    }
}

tls_stream::tls_stream(int fd, const tls_context &ctx) : tcp_stream(fd, nullptr),
                                                         _handshake_ok(false), _fallen_back(false), _chelo_recv(false) {
    _ssl = SSL_new(ctx.ctx());
    if(!_ssl)
        throw RTERR("Failed to create SSL instance");
    _txbio = BIO_new(BIO_s_mem());
    _rxbio = BIO_new(BIO_s_mem());
    if(!_rxbio || !_txbio) {
        SSL_free(_ssl);
        BIO_free(_rxbio);
        BIO_free(_txbio);
        throw RTERR("Failed to create SSL instance");
    }
    BIO_set_close(_rxbio, BIO_CLOSE);
    BIO_set_close(_txbio, BIO_CLOSE);
    SSL_set_bio(_ssl, _rxbio, _txbio);
}

shared_ptr<tls_stream> tls_stream::accept(int fd, const tls_context &ctx) {
    sockaddr_storage sa;
    socklen_t len = sizeof(sa);
    int newFd = ::accept(fd, (sockaddr *)&sa, &len);
    if(newFd < 0)
        throw IOERR(errno);
    shared_ptr<tls_stream> client(new tls_stream(newFd, ctx));
    SSL_set_accept_state(client->_ssl);
    client->_remote_ep = make_shared<ip_endpoint>(&sa);
    return client;
}

void tls_stream::do_handshake() {
    while(!_handshake_ok) {
        int r = SSL_get_error(_ssl, SSL_do_handshake(_ssl));
        if(handle_want(r))
            continue;
        else if(r == SSL_ERROR_NONE) {
            _handshake_ok = true;
            return;
        } else {
            throw RTERR("TLS Error: %s", sslerror_to_string(r));
        }
    }
}

void tls_stream::_put_incoming(const char *buf, int length) {
    if(_chelo_recv) {
        BIO_write(_rxbio, buf, length);
    } else {
        if(buf[0] == 22 && !_fallen_back) {
            BIO_write(_rxbio, buf, length);
            _chelo_recv = true;
        } else {
            stream::buffer->append((void *)buf, length);
            _fallen_back = true;
            _handshake_ok = true;
            SSL_free(_ssl);
            _ssl = nullptr;
        }
    }
}

shared_ptr<message> tls_stream::read(shared_ptr<decoder> decoder) {
    if(_reading_fiber)
        throw RTERR("reading from a stream occupied by another fiber");
    do_handshake();
    if(_fallen_back)
        return stream::read(decoder);
    if(buffer->size() > 0)
        if(decoder->decode(buffer))
            return decoder->msg();
    while(true) {
        char *buf = (char *)buffer->prepare(0x8000);
        int nread = SSL_read(_ssl, buf, 0x8000);
        if(nread < 0) {
            nread = SSL_get_error(_ssl, nread);
            if(handle_want(nread))
                continue;
            else {
                if(nread == SSL_ERROR_NONE)
                    continue;
                throw RTERR("TLS error: %s", sslerror_to_string(nread));
            }
        } else if(nread > 0) {
            try {
                buffer->enlarge(nread);
                if(decoder->decode(buffer))
                    return decoder->msg();
            }
            catch(exception &ex) {
                throw RTERR("Protocol Error: %s", ex.what());
            }
        } else
            handle_want(SSL_ERROR_WANT_READ);
    }
}

void tls_stream::write(const char *buf, int length) {
    do_handshake();
    if(_fallen_back) {
        stream::write(buf, length);
        return;
    }
    while(true) {
        int r = SSL_get_error(_ssl, SSL_write(_ssl, buf, length));
        if(handle_want(r))
            continue;
        else if(r == SSL_ERROR_NONE)
            return;
        throw RTERR("TLS error: %s", sslerror_to_string(r));
    }
}


bool tls_stream::handle_want(int r) {
    int pendingBytes = BIO_ctrl_pending(_txbio);
    if(pendingBytes > 0) {
        char *buf = new char[pendingBytes];
        BIO_read(_txbio, buf, pendingBytes);
        tcp_stream::write(buf, pendingBytes);
        delete buf;
    }
    if(r == SSL_ERROR_WANT_WRITE)
        return true;
    else if(r == SSL_ERROR_WANT_READ) {
        if(!fiber::in_fiber())
            throw logic_error("reading from a stream outside a fiber");
        return true;
    }
    return false;
}

bool tls_stream::has_tls() {
    return !_fallen_back;
}

void tls_stream::connect(const string &host, int port) {
    if(_handshake_ok)
        throw RTERR("TLS socket already connected");
    tcp_stream::connect(host, port);
    SSL_set_connect_state(_ssl);
    _chelo_recv = true;
}

tls_stream::~tls_stream() {
    if(_ssl) SSL_free(_ssl);
}

tls_context::tls_context() {
    _ctx = SSL_CTX_new(TLS_method());
    if(!_ctx)
        throw RTERR("Failed to create SSL CTX instance");
}

tls_context::tls_context(const tls_context &ctx) : _ctx(ctx._ctx) {
    SSL_CTX_up_ref(_ctx);
}

tls_context::tls_context(SSL_CTX *ctx) : _ctx(ctx) {
    SSL_CTX_up_ref(_ctx);
}

void tls_context::use_certificate(const char *file) {
    if(!SSL_CTX_use_certificate_chain_file(_ctx, file))
        throw RTERR("Failed to use certificate file: %s",
                    ERR_reason_error_string(ERR_get_error()));
}

void tls_context::use_certificate(const char *file, const char *key) {
    use_certificate(file);
    SSL_CTX_use_PrivateKey_file(_ctx, key, SSL_FILETYPE_PEM);
}

static int tls_sni_callback(SSL *ssl, int *ad, void *arg) {
    const char* hostName = SSL_get_servername(ssl, TLSEXT_NAMETYPE_host_name);

}

void tls_context::register_context(const string &hostname, tls_context &ctx) {
    if(_others.empty()) {
        SSL_CTX_set_tlsext_servername_callback(_ctx, tls_sni_callback);
        SSL_CTX_set_tlsext_servername_arg(_ctx, this);
    }
    _others[hostname] = ctx;
}

void tls_context::unregister_context(const string &hostname) {
    _others.erase(hostname);
}

tls_context::~tls_context() {
    SSL_CTX_free(_ctx);
}

https_server::https_server(tls_context ctx, shared_ptr<http_service> svc)
        : http_server(svc), _ctx(ctx) {}

https_server::https_server(shared_ptr<http_service> svc) : http_server(svc) {}

https_server::~https_server() {}

tls_context https_server::ctx() {
    return _ctx;
}

void https_server::use_certificate(const char *file, const char *key) {
    _ctx.use_certificate(file, key);
}

void https_server::handle_incoming() {
    try {
        auto client = tls_stream::accept(_fd, _ctx);
        start_thread(shared_ptr<stream>(client),
                     client->getpeername()->straddr());
    }
    catch(exception &ex) {
        cerr<<ex.what()<<endl;
        return;
    }
}