#include <iostream>
#include <cstring>

#include "xyhttp.h"

using namespace std;

http_connection::http_connection(P<stream> strm, string pname)
    : _reqdec(make_shared<http_request::decoder>()), _strm(strm),
      _keep_alive(true), _upgraded(false), _peername(pname) {}

P<http_request> http_connection::next_request() {
    try {
        P<http_request> _req = _strm->read<http_request>(_reqdec);
        chunk connhdr = _req->header("connection");
        if(connhdr) {
            _keep_alive = connhdr.find("keep-alive") != -1 ||
                          connhdr.find("Keep-Alive") != -1;
        } else {
            _keep_alive = false;
        }
        return _req;
    }
    catch(exception &ex) {
        _keep_alive = false;
        _strm.reset();
        return nullptr;
    }
}

bool http_connection::keep_alive() {
    return _keep_alive && _strm && !_upgraded;
}

shared_ptr<stream> http_connection::upgrade() {
    _upgraded = true;
    _strm->set_timeout(0);
    return _strm;
}

void http_connection::invoke_service(const P<http_service> &svc, http_trx &tx) {
    try {
        if(!tx->request->header("host")) {
            tx->display_error(400);
            _strm.reset();
            return;
        }
        svc->serve(tx);
        if(!tx->header_sent())
            tx->display_error(404);
    }
    catch(extended_runtime_error &ex) {
        if(tx->header_sent()) {
            cerr<<"["<<timelabel()<<" "<<peername()<<"] ";
            cerr<<ex.filename()<<":"<<ex.lineno()<<": "<<ex.what()<<endl;
            _strm.reset();
            return;
        }
        auto resp = tx->get_response(500);
        resp->set_header("Content-Type", "text/html");
        tx->write(fmt("<html><head><title>XWSG Internal Error</title></head>"
                      "<body><h1>500 Internal Server Error</h1><p><span>%s:%d:</span> %s</p>"
                      "<ul style=\"color:gray\">", ex.filename(), ex.lineno(), ex.what()));
        char **trace = ex.stacktrace();
        for(int i = 0; i < ex.tracedepth(); i++)
            tx->write(fmt("<li>%s</li>", trace[i]));
        free(trace);
        tx->write(fmt("</ul><i style=\"font-size:.8em\">%s</i></body></html>",
                      http_transaction::SERVER_VERSION.c_str()));
    }
    catch(exception &ex) {
        if(tx->header_sent()) {
            cerr<<"["<<timelabel()<<" "<<peername()<<"] "<<ex.what()<<endl;
            _strm.reset();
            return;
        }
        auto resp = tx->get_response(500);
        resp->set_header("Content-Type", "text/html");
        tx->write(fmt("<html>"
                      "<head><title>XWSG Internal Error</title></head>"
                      "<body><h1>500 Internal Server Error</h1>"
                      "<p>%s</p></body></html>", ex.what()));
    }
    tx->finish();
}

bool http_connection::has_tls() {
    return _strm->has_tls();
}

http_server::http_server(shared_ptr<http_service> svc) : service(svc) {
    _server = mem_alloc<uv_tcp_t>();
    if(uv_tcp_init(uv_default_loop(), _server) < 0) {
        free(_server);
        throw runtime_error("failed to initialize libuv TCP stream");
    }
    _server->data = this;
}

http_server::http_server(http_service *svc)
        : http_server(shared_ptr<http_service>(svc)) {}

static void http_server_on_connection(uv_stream_t* strm, int status) {
    http_server *self = (http_server *)strm->data;
    if(status >= 0) {
        tcp_stream *client = new tcp_stream();
        try {
            client->accept(strm);
            client->nodelay(true);
        }
        catch(exception &ex) {
            delete client;
            return;
        }
        auto connection = make_shared<http_connection>(
                shared_ptr<stream>(client), client->getpeername()->straddr());
        fiber::launch(bind(&http_server::service_loop, self, connection));
    }
}

void http_server::service_loop(P<http_connection> conn) {
    while(conn->keep_alive()) {
        shared_ptr<http_request> request = conn->next_request();
        if(!request) break;
        try {
            conn->invoke_service(service,
                                 make_shared<http_transaction>(conn, move(request)));
        }
        catch(exception &ex) {
            cerr<<"["<<timelabel()<<" "<<conn->peername()<<"] "<<ex.what()<<endl;
            break;
        }
    }
}

void http_server::listen(const char *addr, int port) {
    sockaddr_storage saddr;
    if(uv_ip4_addr(addr, port, (struct sockaddr_in*)&saddr) &&
       uv_ip6_addr(addr, port, (struct sockaddr_in6*)&saddr))
        throw invalid_argument("invalid IP address or port");
    int r = uv_tcp_bind(_server, (struct sockaddr *)&saddr, 0);
    if(r < 0)
        throw runtime_error(uv_strerror(r));
    do_listen(64);
}

void http_server::do_listen(int backlog) {
    int r = uv_listen((uv_stream_t *)_server, backlog, http_server_on_connection);
    if(r < 0)
        throw runtime_error(uv_strerror(r));
}

http_server::~http_server() {
    if(_server) {
        uv_close((uv_handle_t *)_server, (uv_close_cb) free);
    }
}