#include "xyhttpsvc.h"

#include <cstring>
#include <iostream>
#include <sstream>

void http_service::serve(shared_ptr<http_transaction> tx) {
    cout<<"["<<timelabel()<<fmt(" %s] %s %s%s",
        tx->connection->peername()->c_str(),
        tx->request->method_name(),
        tx->request->header("host")->c_str(),
        tx->request->resource()->c_str())<<endl;
    tx->forward_to("222.24.62.120", 80);
    return;
    auto resp = tx->make_response(200);
    const string it_works = "<html>"
        "<head><title>Welcome to XWSG</title></head>"
        "<body><h1>It works!</h1></body></html>";
    resp->set_header("Content-Type", "text/html");
    resp->set_header("Content-Length", to_string(it_works.size()));
    tx->write(it_works);
}

local_file_service::local_file_service(const string &docroot)
    : _docroot(docroot) {
        _defdocs.push_back("index.html");
        _defdocs.push_back("index.php");
        register_mimetype("html", "text/html");
    }

void local_file_service::register_mimetype(const string &ext, const string &type) {
    _mimetypes[ext] = shared_ptr<string>(new string(type));
}

void local_file_service::serve(shared_ptr<http_transaction> tx) {
    const char *requested_res = tx->request->resource()->c_str();
    const char *tail = requested_res;
    struct stat info;
    string pathbuf = _docroot;
    while(requested_res) {
        tail = strchr(tail, '/');
        if(tail == requested_res) {
            tail++;
            continue;
        }
        string pathpart(requested_res,
            tail ? tail - requested_res : strlen(requested_res));
        if(pathpart.size() > 1)
            pathbuf += pathpart;
        if(stat(pathbuf.c_str(), &info) < 0) {
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
        pathbuf += "/";
        for(auto it = _defdocs.begin(); it != _defdocs.end(); it++) {
            string fname = pathbuf + *it;
            if(stat(fname.c_str(), &info) < 0)
                continue;
            if(S_ISREG(info.st_mode)) {
                pathbuf = fname;
                break;
            }
        }
    }
    if(!S_ISREG(info.st_mode))
        return;
    const char *extpos = strrchr(pathbuf.c_str(), '.');
    shared_ptr<http_response> resp = tx->make_response();
    if(extpos) {
        string ext(extpos + 1);
        resp->set_header("Content-Type", _mimetypes[ext]);
    }
    cout<<pathbuf<<endl;
    tx->serve_file(pathbuf);
}