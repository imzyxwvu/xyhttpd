#include <iostream>
#include <cstring>

#include "xyhttp.h"

http_connection::http_connection(
    shared_ptr<http_service> svc, shared_ptr<stream> strm, shared_ptr<string> pname)
    : _reqdec(new http_request::decoder()), _svc(svc), _strm(strm),
      _keep_alive(true), _upgraded(false), _peername(pname) {}

shared_ptr<http_request> http_connection::next_request() {
    shared_ptr<http_request> _req = _strm->read<http_request>(_reqdec);
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

void http_connection::invoke_service(shared_ptr<http_transaction> tx) {
    try {
        _svc->serve(tx);
        if(!tx->header_sent())
            tx->display_error(404);
    }
    catch(extended_runtime_error &ex) {
        cerr<<"["<<timelabel()<<" "<<*peername()<<"] ";
        cerr<<ex.filename()<<":"<<ex.lineno()<<": "<<ex.what()<<endl;
        if(tx->header_sent()) {
            _keep_alive = false;
            return;
        }
        auto resp = tx->make_response(500);
        int pagelen = 240 + strlen(ex.filename()) + strlen(ex.what());
        char **trace = ex.stacktrace();
        for(int i = 0; i < ex.tracedepth(); i++)
            pagelen += 10 + strlen(trace[i]);
        char *page = new char[pagelen];
        char *ptr = page;
        ptr += sprintf(ptr, "<html><head><title>XWSG Internal Error</title></head>"
            "<body><h1>500 Internal Server Error</h1><p><span>%s:%d:</span> %s</p>"
            "<ul style=\"color:gray\">", ex.filename(), ex.lineno(), ex.what());
        for(int i = 0; i < ex.tracedepth(); i++)
            ptr += sprintf(ptr, "<li>%s</li>", trace[i]);
        ptr += sprintf(ptr, "</ul><i style=\"font-size:.8em\">%s</i></body></html>",
            http_transaction::SERVER_VERSION.c_str());
        resp->set_header("Content-Type", "text/html");
        resp->set_header("Content-Length", to_string(ptr - page));
        tx->write(page, ptr - page);
        delete page;
    }
    catch(exception &ex) {
        cerr<<"["<<timelabel()<<" "<<*peername()<<"] ";
        cout<<ex.what()<<endl;
        if(tx->header_sent()) {
            _keep_alive = false;
            return;
        }
        auto resp = tx->make_response(500);
        char *page = new char[128 + strlen(ex.what())];
        int size = sprintf(page, "<html>"
            "<head><title>XWSG Internal Error</title></head>"
            "<body><h1>500 Internal Server Error</h1>"
            "<p>%s</p></body></html>", ex.what());
        resp->set_header("Content-Type", "text/html");
        resp->set_header("Content-Length", to_string(size));
        tx->write(page, size);
        delete page;
    }
}

http_server::http_server(shared_ptr<http_service> svc) : service(svc) {
    stream = (uv_tcp_t *)malloc(sizeof(uv_tcp_t));
    if(uv_tcp_init(uv_default_loop(), stream) < 0) {
        free(stream);
        throw runtime_error("failed to initialize libuv TCP stream");
    }
    stream->data = this;
}

http_server::http_server(http_service *svc)
        : http_server(shared_ptr<http_service>(svc)) {}

static void http_service_loop(void *data) {
    shared_ptr<http_connection> conn((http_connection *)data);
    while(conn->keep_alive()) {
        shared_ptr<http_request> req;
        try {
            req = conn->next_request();
            shared_ptr<http_transaction> tx(
                new http_transaction(conn, req));
            conn->invoke_service(tx);
        }
        catch(exception &ex) {
            break;
        }
    }
}

static void http_server_on_connection(uv_stream_t* strm, int status) {
    http_server *self = (http_server *)strm->data;
    if(status >= 0) {
        tcp_stream *client = new tcp_stream();
        if(client->accept(strm) < 0) {
            delete client;
            return;
        }
        shared_ptr<fiber> f = fiber::make(http_service_loop,
            new http_connection(self->service, shared_ptr<stream>(client),
                                client->getpeername()->straddr()));
        f->resume();
    }
}

void http_server::listen(const char *addr, int port) {
    sockaddr_storage saddr;
    if(uv_ip4_addr(addr, port, (struct sockaddr_in*)&saddr) &&
       uv_ip6_addr(addr, port, (struct sockaddr_in6*)&saddr))
        throw invalid_argument("invalid IP address or port");
    int r = uv_tcp_bind(stream, (struct sockaddr *)&saddr, 0);
    if(r < 0)
        throw runtime_error(uv_strerror(r));
    r = uv_listen((uv_stream_t *)stream, 80, http_server_on_connection);
    if(r < 0)
        throw runtime_error(uv_strerror(r));
}

http_server::~http_server() {
    if(stream) {
        uv_close((uv_handle_t *)stream, (uv_close_cb) free);
    }
}