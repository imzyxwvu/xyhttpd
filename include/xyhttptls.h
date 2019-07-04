#ifndef XYHTTPD_XYHTTPTLS_H
#define XYHTTPD_XYHTTPTLS_H

#include "xyhttp.h"
#include "xytlsstream.h"

class https_server : public http_server {
public:
    https_server(shared_ptr<http_service> svc);
    https_server(http_service *svc);
    virtual ~https_server();

    SSL_CTX *ctx();
    virtual void use_certificate(const char *file, const char *key);
    virtual void do_listen(int backlog);
private:
    SSL_CTX *_ctx;
};

#endif