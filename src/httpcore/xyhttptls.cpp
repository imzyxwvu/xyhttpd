#include <iostream>
#include <openssl/err.h>
#include "xyhttptls.h"

using namespace std;

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

tls_stream::tls_stream(const P<tls_context> &ctx)
        : tls_stream(*ctx) {}

tls_stream::tls_stream(const tls_context &ctx) :
        _handshake_ok(false), _chelo_recv(false) {
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

void tls_stream::accept(uv_stream_t *svr) {
    tcp_stream::accept(svr);
    SSL_set_accept_state(_ssl);
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

void tls_stream::_commit_rx(char *base, int nread) {
    if(!_ssl) {
        stream::_commit_rx(base, nread);
        return;
    }
    if(nread > 0) {
        if(_chelo_recv) {
            BIO_write(_rxbio, base, nread);
        } else if(base[0] == 22) {
            BIO_write(_rxbio, base, nread);
            _chelo_recv = true;
        } else {
            buffer.commit(nread);
            _handshake_ok = true;
            SSL_free(_ssl);
            _ssl = nullptr;
        }
    }
    read_cont.resume(nread);
}

void tls_stream::read(const shared_ptr<decoder> &decoder) {
    if(read_cont) throw RTERR("stream is read-busy");
    do_handshake();
    if(!_ssl) return stream::read(decoder);
    if(buffer.size() > 0 && decoder->decode(buffer))
        return;
    while(true) {
        char *buf = buffer.prepare(0x8000);
        int nread = SSL_read(_ssl, buf, 0x8000);
        if(nread < 0) {
            nread = SSL_get_error(_ssl, nread);
            if(handle_want(nread))
                continue;
            else
                throw RTERR("TLS error: %s", sslerror_to_string(nread));
        } else if(nread > 0) {
            buffer.commit(nread);
            if(decoder->decode(buffer))
                return;
        } else
            handle_want(SSL_ERROR_WANT_READ);
    }
}

void tls_stream::write(const char *buf, int length) {
    do_handshake();
    if(!_ssl) {
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
        stream::write(buf, pendingBytes);
        delete[] buf;
    }
    if(r == SSL_ERROR_WANT_WRITE)
        return true;
    else if(r == SSL_ERROR_WANT_READ) {
        auto status = _do_read();
        if(status < 0)
            throw IOERR(status);
        return true;
    }
    return false;
}

bool tls_stream::has_tls() {
    return _ssl != NULL;
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
#if OPENSSL_VERSION_NUMBER < 0x10100000L
    _ctx = SSL_CTX_new(SSLv23_method());
#else
    _ctx = SSL_CTX_new(TLS_method());
#endif
    if(!_ctx)
        throw RTERR("Failed to create SSL CTX instance");
    SSL_CTX_set_options(_ctx, SSL_OP_NO_SSLv2 | SSL_OP_NO_SSLv3);
}

tls_context::tls_context(const char *file, const char *key) : tls_context() {
    use_certificate(file, key);
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

int tls_context::sni_callback(SSL *ssl, int *ad, void *arg) {
    if (ssl == NULL)
        return SSL_TLSEXT_ERR_NOACK;

    const char *hostNamePtr = SSL_get_servername(ssl, TLSEXT_NAMETYPE_host_name);
    if(hostNamePtr == NULL)
        return SSL_TLSEXT_ERR_OK; // No hostname (eg. connect via IP address)

    string hostName(hostNamePtr);
    tls_context *self = static_cast<tls_context *>(arg);
    if(self->_others.find(hostName) != self->_others.end())
        SSL_set_SSL_CTX(ssl, self->_others[hostName]->_ctx);
    return SSL_TLSEXT_ERR_OK;
}

void tls_context::register_context(const string &hostname, P<tls_context> ctx) {
    if(_others.empty()) {
        SSL_CTX_set_tlsext_servername_callback(_ctx, tls_context::sni_callback);
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

https_server::https_server(P<tls_context> ctx, P<http_service> svc)
        : http_server(move(svc)), _ctx(move(ctx)) {}

https_server::https_server(P<http_service> svc)
        : http_server(move(svc)), _ctx(make_shared<tls_context>()) {}

https_server::~https_server() {}

P<tls_context> https_server::ctx() {
    return _ctx;
}

void https_server::use_certificate(const char *file, const char *key) {
    _ctx->use_certificate(file, key);
}

static void https_server_on_connection(uv_stream_t* strm, int status) {
    https_server *self = (https_server *)strm->data;
    if(status >= 0) {
        P<tls_stream> client = make_shared<tls_stream>(*self->ctx());
        try {
            client->accept(strm);
            client->nodelay(true);
            // See comments in http_server_on_connection() from xyhttp.cpp
            P<ip_endpoint> peer = client->getpeername();
            fiber::launch(bind(&https_server::service_loop, self,
                               make_shared<http_connection>(client, peer->straddr())));
        }
        catch(exception &ex) {
            return;
        }
    }
}

void https_server::do_listen(int backlog) {
    int r = uv_listen((uv_stream_t *)_server, backlog, https_server_on_connection);
    if(r < 0)
        throw runtime_error(uv_strerror(r));
}