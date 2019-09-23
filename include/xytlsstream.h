#ifndef XYHTTPD_XYTLSSTREAM_H
#define XYHTTPD_XYTLSSTREAM_H

#include "xystream.h"

#include <unordered_map>
#include <openssl/ssl.h>

class tls_context {
public:
    tls_context();
    tls_context(const char *file, const char *key);
    tls_context(const tls_context &) = delete;
    virtual ~tls_context();
    virtual void register_context(const std::string &hostname, P<tls_context> ctx);
    virtual void unregister_context(const std::string &hostname);
    virtual void use_certificate(const char *file);
    virtual void use_certificate(const char *file, const char *key);
    inline SSL_CTX *ctx() const { return _ctx; }
private:
    SSL_CTX *_ctx;
    std::unordered_map<std::string, P<tls_context>> _others;
    static int sni_callback(SSL *ssl, int *ad, void *arg);
};

class tls_stream : public tcp_stream {
public:
    tls_stream() = delete;
    explicit tls_stream(const P<tls_context> &ctx);
    explicit tls_stream(const tls_context &ctx);
    virtual ~tls_stream();

    virtual void connect(const std::string &host, int port);
    virtual P<message> read(const P<decoder> &);
    virtual void write(const char *buf, int length);
    virtual void accept(uv_stream_t *);
    virtual bool has_tls();
private:
    virtual void _commit_rx(char *base, int nread);
    SSL *_ssl;
    BIO *_txbio, *_rxbio;
    bool _handshake_ok, _chelo_recv;

    void do_handshake();
    bool handle_want(int r);
};

#endif
