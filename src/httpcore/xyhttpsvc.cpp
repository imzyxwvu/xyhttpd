#include "xyhttpsvc.h"

#include <cstring>
#include <iostream>

using namespace std;

http_service::~http_service() {}

void http_service_chain::serve(http_trx &tx) {
    for(auto it = _svcs.cbegin(); it != _svcs.cend(); it++) {
        (*it)->serve(tx);
        if(tx->header_sent()) break;
    }
}

void http_service_chain::append(P<http_service> svc) {
    _svcs.push_back(svc);
}

shared_ptr<http_service_chain> http_service_chain::build(
        const function<void(http_service_chain *)> &builder) {
    auto chain = make_shared<http_service_chain>();
    builder(chain.get());
    return chain;
}

void http_service_chain::match_router::serve(http_trx &tx) {
    if(tx->request->path() == _matchPath)
        _service->serve(tx);
}

http_service_chain::match_router::~match_router() {}

local_file_service::local_file_service(const string &docroot) {
    set_document_root(docroot);
}

void local_file_service::set_document_root(const string &docroot) {
    char absDocRoot[PATH_MAX];
#ifdef _WIN32
    if(_fullpath(absDocRoot, docroot.c_str(), sizeof(absDocRoot))) {
#else
    if(realpath(docroot.c_str(), absDocRoot)) {
#endif
        _docroot = absDocRoot;
    } else {
        throw RTERR("failed to resolve document root");
    }
}

void local_file_service::add_default_name(const string &defdoc) {
    _defdocs.push_back(defdoc);
}

void local_file_service::register_mimetype(const string &ext, chunk type) {
    int base = 0;
    while(base < ext.size()) {
        int till = ext.find(' ', base);
        if(till == string::npos) {
            _mimetypes[ext.substr(base)] = type;
            break;
        } else {
            _mimetypes[ext.substr(base, till-base)] = type;
            base = till + 1;
        }
    }
}

void local_file_service::register_fcgi(const string &ext, shared_ptr<fcgi_provider> provider) {
    _fcgi_providers[ext] = provider;
}

void local_file_service::serve(http_trx &tx) {
    const char *requested_res = tx->request->path().data();
    const char *tail = requested_res;
    struct stat info;
    string pathbuf, fullpathbuf;
    while(requested_res) {
        tail = strchr(tail, '/');
        if(tail == requested_res) {
            tail++;
            continue;
        }
        string pathpart(requested_res,
            tail ? tail - requested_res : strlen(requested_res));
        if(pathpart.size() > 0) {
            pathbuf += pathpart;
            fullpathbuf = _docroot + pathbuf;
        }
        if(stat(fullpathbuf.c_str(), &info) < 0) {
            switch(errno) {
                case EACCES: tx->display_error(403); return;
                default: return;
            }
        }
        if(S_ISDIR(info.st_mode)) {
            requested_res = tail;
            continue;
        }
        else if(S_ISREG(info.st_mode))
            break;
        else {
            tx->display_error(403);
            return;
        }
    }
    if(S_ISDIR(info.st_mode)) {
        if(tx->request->resource()[tx->request->resource().size() - 1] != '/') {
            tx->redirect_to(tx->request->path() + '/');
            return;
        }
        fullpathbuf += "/";
        for(auto it = _defdocs.begin(); it != _defdocs.end(); it++) {
            string fname = fullpathbuf + *it;
            if(stat(fname.c_str(), &info) < 0)
                continue;
            if(S_ISREG(info.st_mode)) {
                pathbuf = pathbuf + "/" + *it;
                fullpathbuf = fname;
                break;
            }
        }
    }
    if(!S_ISREG(info.st_mode))
        return;
    const char *extpos = strrchr(pathbuf.c_str(), '.');
    if(extpos) {
        string ext(extpos + 1);
        shared_ptr<fcgi_provider> fcgiProvider = _fcgi_providers[ext];
        if(fcgiProvider) {
            shared_ptr<fcgi_connection> conn;
            try {
                conn = fcgiProvider->get_connection();
            }
            catch(runtime_error &ex) {
                tx->display_error(502);
                return;
            }
            conn->set_env("SCRIPT_FILENAME", fullpathbuf);
            conn->set_env("DOCUMENT_ROOT", _docroot);
            conn->set_env("SCRIPT_NAME", pathbuf);
            tx->forward_to(conn);
            return;
        } else {
            shared_ptr<http_response> resp = tx->get_response();
            resp->set_header("Content-Type", _mimetypes[ext]);
        }
    }
    tx->serve_file(fullpathbuf);
}

logger_service::logger_service(ostream *os) : _os(*os) {}

void logger_service::serve(http_trx &tx) {
    if(tx->request->resource()[0] == '/') {
        _os<<"["<<timelabel()<<fmt(" %s] %s %s%s",
                                   tx->connection->peername().c_str(),
                                   tx->request->method.c_str(),
                                   tx->request->header("host").data(),
                                   tx->request->resource().data())<<endl;
    } else {
        _os<<"["<<timelabel()<<fmt(" %s] %s %s",
                                   tx->connection->peername().c_str(),
                                   tx->request->method.c_str(),
                                   tx->request->resource().data())<<endl;
    }
}

tls_filter_service::tls_filter_service(int code) : _code(code) {}

void tls_filter_service::serve(http_trx &tx) {
    if(!tx->connection->has_tls()) {
        if(_code == 302) {
            tx->redirect_to(fmt("https://%s%s",
                    tx->request->header("host").data(),
                    tx->request->resource().data()));
        } else {
            auto resp = tx->get_response(_code);
            resp->set_header("Content-Type", "text/html");
            tx->write(fmt("<!DOCTYPE html><html><head>"
                          "<title>XWSG TLS Error %d</title></head>"
                          "<body><h1>%d %s</h1><p>"
                          "An HTTP request was sent to an HTTPS port."
                          "</p></body></html>",
                          _code, _code, http_response::state_description(_code)));
            tx->finish();
        }
    }
}

void host_dispatch_service::register_host(const string &hostname, shared_ptr<http_service> svc) {
    if(!svc) {
        unregister_host(hostname);
        return;
    }
    _svcmap[hostname] = svc;
    if(!_default)
        _default = svc;
}

void host_dispatch_service::unregister_host(const string &hostname) {
    _svcmap.erase(hostname);
}

void host_dispatch_service::set_default(shared_ptr<http_service> svc) {
    if(!svc)
        throw RTERR("provided service is null");
    _default = svc;
}

string host_dispatch_service::normalize_hostname(chunk hostname) {
    char buf[512];
    if(hostname.size() > sizeof(buf) - 1)
        throw RTERR("hostname too long");
    strcpy(buf, hostname.data());
    char *portBase = strchr(buf, ':');
    if(portBase) *portBase = 0;
    int length = strlen(buf);
    if(buf[length - 1] == '.') {
        buf[length - 1] = 0;
        length--;
    }
    return string(buf, length);
}

void host_dispatch_service::serve(http_trx &tx) {
    auto host = tx->request->header("host");
    if(!host) {
        tx->display_error(400);
        return;
    }
    string hostName = normalize_hostname(host);
    auto svc = _svcmap[hostName];
    if(svc)
        svc->serve(tx);
    else if(_default)
        _default->serve(tx);
}

proxy_pass_service::proxy_pass_service() : _cur(0) {}

proxy_pass_service::proxy_pass_service(const string &host, int port) : _cur(0) {
    append(host, port);
}

void proxy_pass_service::append(shared_ptr<ip_endpoint> ep) {
    _svcs.push_back(ep);
}

void proxy_pass_service::append(const string &host, int port) {
    append(make_shared<ip_endpoint>(host, port));
}

void proxy_pass_service::serve(http_trx &tx) {
    if(count() == 0)
        return;
    if(_cur >= count())
        _cur = 0;
    auto upstream = make_shared<tcp_stream>();
    upstream->connect(_svcs[_cur++]);
    tx->forward_to(upstream);
}

lambda_service::lambda_service(const function<void(http_trx &)> &func)
: _func(func) {}

lambda_service::lambda_service(const function<void(shared_ptr<websocket>)> &func)
: _ws_func(func) {}

void lambda_service::serve(http_trx &tx) {
    if(_ws_func && tx->request->header("sec-websocket-key")) {
        auto ws = tx->accept_websocket();
        if (!ws) {
            tx->display_error(500);
            return;
        }
        _ws_func(move(ws));
    }
    else if(_func)
        _func(tx);
}

void connect_proxy::serve(http_trx &tx) {
    if(tx->request->method == "CONNECT") {
        const char *path = tx->request->path().data();
        const char *portBase = strchr(path, ':');
        if(portBase == NULL) {
            tx->display_error(403);
            return;
        }
        string hostName = string(path, portBase - path);
        auto endpoint = make_shared<ip_endpoint>(hostName, atoi(portBase));
        auto remote = make_shared<tcp_stream>();
        remote->connect(endpoint);
        auto client = tx->upgrade(false);
        client->write("HTTP/1.1 200 Connection Established\r\n\r\n");
        client->pipe(remote);
        remote->pipe(client);
    }
}
