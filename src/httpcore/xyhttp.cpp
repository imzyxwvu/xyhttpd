#include <iostream>
#include <cstring>

#include "xyhttp.h"

http_connection::http_connection(
    shared_ptr<http_service> svc, shared_ptr<stream> strm, shared_ptr<string> pname)
    : _reqdec(new http_request::decoder()), _svc(svc), _strm(strm),
      _keep_alive(true), _upgraded(false), _peername(pname) {}

shared_ptr<http_request> http_connection::next_request() {
    shared_ptr<http_request> _req = _strm->read<http_request>(_reqdec);
    if(!_req) {
        _keep_alive = false;
        return nullptr;
    }
    shared_ptr<string> connhdr = _req->header("connection");
    if(connhdr) {
        _keep_alive = connhdr->find("keep-alive", 0, 10) != string::npos ||
                      connhdr->find("Keep-Alive", 0, 10) != string::npos;
    } else {
        _keep_alive = false;
    }
    return _req;
}

bool http_connection::keep_alive() {
    return _keep_alive && !_upgraded;
}

shared_ptr<stream> http_connection::upgrade() {
    _upgraded = true;
    return _strm;
}

void http_connection::invoke_service(shared_ptr<http_transaction> tx) {
    try {
        if(!tx->request->header("host"))
            tx->display_error(400);
        _svc->serve(tx);
        if(!tx->header_sent())
            tx->display_error(404);
    }
    catch(extended_runtime_error &ex) {
        if(tx->header_sent()) {
            cerr<<"["<<timelabel()<<" "<<*peername()<<"] ";
            cerr<<ex.filename()<<":"<<ex.lineno()<<": "<<ex.what()<<endl;
            _keep_alive = false;
            return;
        }
        auto resp = tx->get_response(500);
        char **trace = ex.stacktrace();
        resp->set_header("Content-Type", "text/html");
        tx->write(fmt("<html><head><title>XWSG Internal Error</title></head>"
                      "<body><h1>500 Internal Server Error</h1><p><span>%s:%d:</span> %s</p>"
                      "<ul style=\"color:gray\">", ex.filename(), ex.lineno(), ex.what()));
        for(int i = 0; i < ex.tracedepth(); i++)
            tx->write(fmt("<li>%s</li>", trace[i]));
        tx->write(fmt("</ul><i style=\"font-size:.8em\">%s</i></body></html>",
                      http_transaction::SERVER_VERSION.c_str()));
    }
    catch(exception &ex) {
        if(tx->header_sent()) {
            cerr<<"["<<timelabel()<<" "<<*peername()<<"] "<<ex.what()<<endl;
            _keep_alive = false;
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
    _server = (uv_tcp_t *)malloc(sizeof(uv_tcp_t));
    if(uv_tcp_init(uv_default_loop(), _server) < 0) {
        free(_server);
        throw runtime_error("failed to initialize libuv TCP stream");
    }
    _server->data = this;
}

http_server::http_server(http_service *svc)
        : http_server(shared_ptr<http_service>(svc)) {}

static void http_service_loop(void *data) {
    shared_ptr<http_connection> conn((http_connection *)data);
    while(conn->keep_alive()) {
        shared_ptr<http_request> req;
        try {
            req = conn->next_request();
            if(!req) break;
            shared_ptr<http_transaction> tx(new http_transaction(conn, req));
            conn->invoke_service(tx);
        }
        catch(exception &ex) {
            cerr<<"["<<timelabel()<<" "<<conn->peername()->c_str()<<"] "
                <<ex.what()<<endl;
            break;
        }
    }
}

static void http_server_on_connection(uv_stream_t* strm, int status) {
    http_server *self = (http_server *)strm->data;
    if(status >= 0) {
        tcp_stream *client = new tcp_stream();
        try {
            client->accept(strm);
        }
        catch(exception &ex) {
            delete client;
            return;
        }
        self->start_thread(shared_ptr<stream>(client),
                           client->getpeername()->straddr());
    }
}

void http_server::start_thread(shared_ptr<stream> strm, shared_ptr<string> pname) {
    shared_ptr<fiber> f = fiber::make(http_service_loop,
            new http_connection(service, strm, pname));
    f->resume();
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