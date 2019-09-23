#ifndef XYHTTPD_HTTPSVC_H
#define XYHTTPD_HTTPSVC_H

#include "xyhttp.h"
#include "xyfcgi.h"
#include <vector>
#include <ostream>

class http_service_chain : public http_service {
public:
    class match_router : public http_service {
    public:
        match_router(std::string path, P<http_service> svc)
            : _matchPath(std::move(path)), _service(std::move(svc)) {}
        virtual void serve(http_trx &tx);
        virtual ~match_router();
    private:
        std::string _matchPath;
        P<http_service> _service;
    };

    virtual void serve(http_trx &tx);
    virtual void append(P<http_service> svc);
    template<typename _Tp, typename... _Args>
    inline void append(_Args&&... __args) {
        append(std::make_shared<_Tp>(std::forward<_Args>(__args)...));
    }
    template<typename _Tp, typename... _Args>
    inline void route(const std::string &path, _Args&&... __args) {
        append(std::make_shared<match_router>(path,
                std::make_shared<_Tp>(std::forward<_Args>(__args)...)));
    }
    inline P<http_service> &operator[](int i) {
        return _svcs.at(i);
    }
    inline int size() const {
        return _svcs.size();
    }
    static P<http_service_chain>
        build(const std::function<void(http_service_chain *)> &builder);
private:
    std::vector<P<http_service>> _svcs;
};

class local_file_service : public http_service {
public:
    explicit local_file_service(const std::string &docroot);
    inline chunk document_root() { return _docroot; }
    void set_document_root(const std::string &docroot);
    void add_default_name(const std::string &defdoc);
    void register_mimetype(const std::string &ext, chunk type);
    void register_fcgi(const std::string &ext, P<fcgi_provider> provider);
    virtual void serve(http_trx &tx);
private:
    std::string _docroot;
    std::vector<std::string> _defdocs;
    std::unordered_map<std::string, chunk> _mimetypes;
    std::unordered_map<std::string, P<fcgi_provider>> _fcgi_providers;
};

class logger_service : public http_service {
public:
    explicit logger_service(std::ostream *os);
    virtual void serve(http_trx &tx);
private:
    std::ostream &_os;
};

class tls_filter_service : public http_service {
public:
    tls_filter_service(int code);
    virtual void serve(http_trx &tx);
private:
    int _code;
};

class host_dispatch_service : public http_service {
public:
    host_dispatch_service();
    void register_host(const std::string &hostname, P<http_service> svc);
    void unregister_host(const std::string &hostname);
    void set_default(P<http_service> svc);
    static std::string normalize_hostname(chunk hostname);
    virtual void serve(http_trx &tx);
private:
    P<http_service> _default;
    std::unordered_map<std::string, P<http_service>> _svcmap;
};

class proxy_pass_service : public http_service {
public:
    proxy_pass_service();
    proxy_pass_service(const std::string &host, int port);
    virtual void serve(http_trx &tx);
    virtual void append(P<ip_endpoint> ep);
    virtual void append(const std::string &host, int port);
    inline P<ip_endpoint> &operator[](int i) {
        return _svcs.at(i);
    }
    inline int count() const {
        return _svcs.size();
    }
private:
    std::vector<P<ip_endpoint>> _svcs;
    int _cur;
};

class lambda_service : public http_service {
public:
    lambda_service(const std::function<void(http_trx &)> &func);
    lambda_service(const std::function<void(P<websocket>)> &func);
    virtual void serve(http_trx &tx);
private:
    std::function<void(http_trx &)> _func;
    std::function<void(P<websocket>)> _ws_func;
};

class connect_proxy : public http_service {
public:
    virtual void serve(http_trx &tx);
};

#endif