#include "xyhttpsvc.h"

#include <cstring>
#include <iostream>
#include <sstream>

void http_service::serve(shared_ptr<http_transaction> tx) {
    auto resp = tx->make_response(200);
    const string it_works = "<html>"
        "<head><title>Welcome to XWSG</title></head>"
        "<body><h1>It works!</h1></body></html>";
    resp->set_header("Content-Type", "text/html");
    resp->set_header("Content-Length", to_string(it_works.size()));
    tx->write(it_works);
}

void http_service_chain::serve(shared_ptr<http_transaction> tx) {
    for(auto it = _svcs.cbegin(); it != _svcs.cend(); it++) {
        (*it)->serve(tx);
        if(tx->header_sent()) break;
    }
}

void http_service_chain::append(shared_ptr<http_service> svc) {
    _svcs.push_back(svc);
}

http_service_chain::~http_service_chain() {}

local_file_service::local_file_service(const string &docroot)
    : _docroot(nullptr) {
        char absDocRoot[PATH_MAX];
        if(realpath(docroot.c_str(), absDocRoot)) {
            _docroot = make_shared<string>(absDocRoot);
        }
        _defdocs.push_back("index.html");
        _defdocs.push_back("index.php");
        register_mimetype("html", "text/html");

    }

void local_file_service::add_defdoc_name(const string &defdoc) {
    _defdocs.push_back(defdoc);
}

void local_file_service::register_mimetype(const string &ext, const string &type) {
    _mimetypes[ext] = make_shared<string>(type);
}

void local_file_service::register_mimetype(const string &ext, shared_ptr<string> type) {
    _mimetypes[ext] = type;
}

void local_file_service::register_fcgi(const string &ext, shared_ptr<fcgi_provider> provider) {
    _fcgi_providers[ext] = provider;
}

void local_file_service::serve(shared_ptr<http_transaction> tx) {
    const char *requested_res = tx->request->path()->c_str();
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
        if(pathpart.size() > 1) {
            pathbuf += pathpart;
            fullpathbuf = *_docroot + pathbuf;
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
        if(tx->request->resource()->back() != '/') {
            tx->redirect_to(*(tx->request->resource()) + '/');
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
            shared_ptr<http_response> resp = tx->make_response();
            resp->set_header("Content-Type", _mimetypes[ext]);
        }
    }
    tx->serve_file(fullpathbuf);
}

logger_service::logger_service(ostream &os) : _os(os) {}

void logger_service::serve(shared_ptr<http_transaction> tx) {
    _os<<"["<<timelabel()<<fmt(" %s] %s %s%s",
            tx->connection->peername()->c_str(),
            tx->request->method_name(),
            tx->request->header("host")->c_str(),
            tx->request->resource()->c_str())<<endl;
}

tls_filter_service::tls_filter_service(int code) : _code(code) {}

void tls_filter_service::serve(shared_ptr<http_transaction> tx) {
    if(!tx->connection->has_tls()) {
        if(_code == 302) {
            tx->redirect_to(fmt("https://%s%s",
                    tx->request->header("host")->c_str(),
                    tx->request->resource()->c_str()));
        } else {
            auto str = fmt("<!DOCTYPE html><html><head>"
                           "<title>XWSG TLS Error %d</title></head>"
                           "<body><h1>%d %s</h1><p>"
                           "An HTTP request was sent to an HTTPS port."
                           "</p></body></html>",
                           _code, _code, http_response::state_description(_code));
            auto resp = tx->make_response(_code);
            resp->set_header("Content-Type", "text/html");
            resp->set_header("Content-Length", to_string(str.size()));
            tx->flush_response();
            tx->write(str.data(), str.size());
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

string host_dispatch_service::normalize_hostname(shared_ptr<string> hostname) {
    char buf[NAME_MAX];
    if(hostname->size() > NAME_MAX - 1)
        throw RTERR("hostname too long");
    strcpy(buf, hostname->c_str());
    char *portBase = strchr(buf, ':');
    if(portBase) *portBase = 0;
    int length = strlen(buf);
    if(buf[length - 1] == '.') {
        buf[length - 1] = 0;
        length--;
    }
    return string(buf, length);
}

void host_dispatch_service::serve(shared_ptr<http_transaction> tx) {
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