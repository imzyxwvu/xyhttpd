#ifndef XYHTTPD_HTTPSVC_H
#define XYHTTPD_HTTPSVC_H

#include "xyhttp.h"
#include "xyfcgi.h"
#include <vector>
#include <ostream>
#include <regex>

class http_service_chain : public http_service {
public:
    virtual void serve(shared_ptr<http_transaction> tx);
    virtual void append(shared_ptr<http_service> svc);
    template<typename _Tp, typename... _Args>
    inline void append(_Args&&... __args) {
        append(make_shared<_Tp>(std::forward<_Args>(__args)...));
    }
    virtual void route(const string &r, shared_ptr<http_service> svc);
    template<typename _Tp, typename... _Args>
    inline void route(const string &r, _Args&&... __args) {
        route(r, make_shared<_Tp>(std::forward<_Args>(__args)...));
    }
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

class proxy_pass_service : public http_service {
public:
    proxy_pass_service();
    virtual void serve(shared_ptr<http_transaction> tx);
    virtual void append(shared_ptr<ip_endpoint> ep);
    virtual void append(const string &host, int port);
    inline shared_ptr<ip_endpoint> &operator[](int i) {
        return _svcs.at(i);
    }
    inline int count() const {
        return _svcs.size();
    }
private:
    vector<shared_ptr<ip_endpoint>> _svcs;
    int _cur;
};

class plain_data_service : public http_service {
public:
    plain_data_service(const string &data);
    plain_data_service(const string &data, const string &ctype);
    void update_data(const string &data);
    virtual void serve(shared_ptr<http_transaction> tx);
private:
    void update_etag();
    string _data, _ctype, _etag;
};

class regex_route : public http_service {
public:
    explicit regex_route(const string &pat, shared_ptr<http_service> svc);
    explicit regex_route(const regex &pat, shared_ptr<http_service> svc);
    virtual void serve(shared_ptr<http_transaction> tx);
private:
    regex _pattern;
    shared_ptr<http_service> _svc;
};

#endif