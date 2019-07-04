#ifndef XYHTTPD_HTTPSVC_H
#define XYHTTPD_HTTPSVC_H

#include "xyhttp.h"
#include "xyfcgi.h"
#include <vector>
#include <ostream>

class http_service_chain : public http_service {
public:
    ~http_service_chain();

    virtual void serve(shared_ptr<http_transaction> tx);
    virtual void append(shared_ptr<http_service> svc);
    inline shared_ptr<http_service> &operator[](int i) {
        return _svcs.at(i);
    }
    inline int size() const {
        return _svcs.size();
    }
private:
    vector<shared_ptr<http_service>> _svcs;
};

class local_file_service : public http_service {
public:
    local_file_service(const string &docroot);
    inline shared_ptr<string> document_root() {
        return _docroot;
    }
    void set_document_root(const string &docroot);
    void add_defdoc_name(const string &defdoc);
    void register_mimetype(const string &ext, const string &type);
    void register_mimetype(const string &ext, shared_ptr<string> type);
    void register_fcgi(const string &ext, shared_ptr<fcgi_provider> provider);
    virtual void serve(shared_ptr<http_transaction> tx);
private:
    shared_ptr<string> _docroot;
    vector<string> _defdocs;
    map<string, shared_ptr<string>> _mimetypes;
    map<string, shared_ptr<fcgi_provider>> _fcgi_providers;
};

class logger_service : public http_service {
public:
    logger_service(ostream &os);
    virtual void serve(shared_ptr<http_transaction> tx);
private:
    ostream &_os;
};

class tls_filter_service : public http_service {
public:
    tls_filter_service(int code);
    virtual void serve(shared_ptr<http_transaction> tx);
private:
    int _code;
};

class host_dispatch_service : public http_service {
public:
    host_dispatch_service();
    void register_host(const string &hostname, shared_ptr<http_service> svc);
    void unregister_host(const string &hostname);
    void set_default(shared_ptr<http_service> svc);
    static string normalize_hostname(shared_ptr<string> hostname);
    virtual void serve(shared_ptr<http_transaction> tx);
private:
    shared_ptr<http_service> _default;
    map<string, shared_ptr<http_service>> _svcmap;
};

#endif