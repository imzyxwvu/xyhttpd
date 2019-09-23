#include <iostream>
#include <openssl/err.h>
#include "xyhttptls.h"

using namespace std;

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
        tls_stream *client = new tls_stream(*self->ctx());
        try {
            client->accept(strm);
            client->nodelay(true);
        }
        catch(exception &ex) {
            delete client;
            cerr<<ex.what()<<endl;
            return;
        }
        auto connection = make_shared<http_connection>(
                shared_ptr<stream>(client), client->getpeername()->straddr());
        fiber::launch(bind(&https_server::service_loop, self, connection));
    }
}

void https_server::do_listen(int backlog) {
    int r = uv_listen((uv_stream_t *)_server, backlog, https_server_on_connection);
    if(r < 0)
        throw runtime_error(uv_strerror(r));
}