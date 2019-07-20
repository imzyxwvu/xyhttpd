#ifndef XYHTTPD_XYTLSSTREAM_H
#define XYHTTPD_XYTLSSTREAM_H

#include "xystream.h"

#include <openssl/ssl.h>

class tls_context {
public:
    tls_context();
    tls_context(const tls_context &);
    tls_context(SSL_CTX *ctx);
    virtual ~tls_context();
    virtual void register_context(const string &hostname, tls_context &ctx);
    virtual void unregister_context(const string &hostname);
    virtual void use_certificate(const char *file);
    virtual void use_certificate(const char *file, const char *key);
    inline SSL_CTX *ctx() const { return _ctx; }
private:
    SSL_CTX *_ctx;
    map<string, tls_context> _others;
};

class tls_stream : public tcp_stream {
public:
    explicit tls_stream(const tls_context &ctx);
    tls_stream(int fd, const tls_context &ctx);
    virtual ~tls_stream();

    void _put_incoming(const char *buf, int length);
    virtual void connect(const string &host, int port);
    virtual shared_ptr<message> read(shared_ptr<decoder>);
    virtual void write(const char *buf, int length);
    static shared_ptr<tls_stream> accept(int fd, const tls_context &ctx);
    virtual bool has_tls();
private:
    SSL *_ssl;
    BIO *_txbio, *_rxbio;
    int _eof_status;
    bool _handshake_ok, _fallen_back, _chelo_recv;

    tls_stream();
    tls_stream(int fd, shared_ptr<ip_endpoint> ep);
    void do_handshake();
    bool handle_want(int r);
};

#endif
