#ifndef XYHTTPD_XYTLSSTREAM_H
#define XYHTTPD_XYTLSSTREAM_H

#include "xystream.h"

#include <openssl/ssl.h>

class tls_stream : public tcp_stream {
public:
    tls_stream(SSL_CTX *ctx);
    virtual ~tls_stream();

    void _put_incoming(const char *buf, int length);
    virtual shared_ptr<message> read(shared_ptr<decoder>);
    virtual void write(const char *buf, int length);
    virtual void accept(uv_stream_t *);
    virtual void do_handshake();
    virtual bool has_tls();
private:
    SSL *_ssl;
    BIO *_txbio, *_rxbio;
    int _eof_status;
    bool _handshake_ok, _fallen_back, _chelo_recv;

    tls_stream();
    bool handle_want(int r);
};

#endif
