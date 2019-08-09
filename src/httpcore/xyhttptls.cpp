#include <iostream>
#include <openssl/err.h>
#include "xyhttptls.h"

tls_context::tls_context() {
#if OPENSSL_VERSION_NUMBER < 0x10100000L
    _ctx = SSL_CTX_new(TLSv1_method());
#else
    _ctx = SSL_CTX_new(TLS_method());
#endif
    if(!_ctx)
        throw RTERR("Failed to create SSL CTX instance");
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

void tls_context::register_context(const string &hostname, shared_ptr<tls_context> ctx) {
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

https_server::https_server(shared_ptr<tls_context> ctx, shared_ptr<http_service> svc)
        : http_server(move(svc)), _ctx(move(ctx)) {}

https_server::https_server(shared_ptr<http_service> svc)
        : http_server(move(svc)), _ctx(make_shared<tls_context>()) {}

https_server::~https_server() {}

shared_ptr<tls_context> https_server::ctx() {
    return _ctx;
}

void https_server::use_certificate(const char *file, const char *key) {
    _ctx->use_certificate(file, key);
}

static void https_server_on_connection(uv_stream_t* strm, int status) {
    https_server *self = (https_server *)strm->data;
    if(status >= 0) {
        tls_stream *client = new tls_stream(*self->ctx());
        try {
            client->accept(strm);
        }
        catch(exception &ex) {
            delete client;
            cerr<<ex.what()<<endl;
            return;
        }
        self->start_thread(shared_ptr<stream>(client),
                           client->getpeername()->straddr());
    }
}

void https_server::do_listen(int backlog) {
    int r = uv_listen((uv_stream_t *)_server, backlog, https_server_on_connection);
    if(r < 0)
        throw runtime_error(uv_strerror(r));
}