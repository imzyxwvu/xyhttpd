#ifndef XYHTTPD_XYHTTPTLS_H
#define XYHTTPD_XYHTTPTLS_H

#include "xyhttp.h"
#include "xytlsstream.h"

class https_server : public http_server {
public:
    explicit https_server(P<http_service> svc);
    https_server(P<tls_context> ctx, P<http_service> svc);
    virtual ~https_server();

    P<tls_context> ctx();
    virtual void use_certificate(const char *file, const char *key);
    virtual void do_listen(int backlog);
private:
    P<tls_context> _ctx;
};

#endif