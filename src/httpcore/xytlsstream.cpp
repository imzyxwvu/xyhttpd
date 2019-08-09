#include <iostream>
#include <openssl/err.h>
#include "xytlsstream.h"

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

tls_stream::tls_stream(const shared_ptr<tls_context> &ctx)
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

static void tls_stream_on_write(uv_write_t *req, int status)
{
    free(req->data);
    free(req);
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
    reading_fiber->resume(int_status::make(nread));
}

shared_ptr<message> tls_stream::read(const shared_ptr<decoder> &decoder) {
    if(reading_fiber) throw RTERR("stream is read-busy");
    do_handshake();
    if(!_ssl) return stream::read(decoder);
    if(buffer.size() > 0)
        if(decoder->decode(buffer))
            return decoder->msg();
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
            try {
                buffer.commit(nread);
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
    if(reading_fiber) throw RTERR("half-duplex stream is read-busy");
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
        auto s = _do_read();
        if(s->status() < 0)
            throw IOERR(s->status());
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
