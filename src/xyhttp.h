#ifndef XYHTTPD_HTTP_H
#define XYHTTPD_HTTP_H

#include "xycommon.h"
#include "xyfiber.h"
#include "xystream.h"

#include <map>
#include <uv.h>

enum http_method {
    GET, HEAD, POST, PUT, DELETE, OPTIONS,
    CONNECT, // HTTP Tunnelling
    BREW, // Hypertext Coffee Pot Control Protocol
    M_SEARCH, // Universal PnP Multicast Search
};

class http_request : public message {
public:
    http_request();
    inline http_method method();
    inline shared_ptr<string> resource() const { return _resource; }
    inline shared_ptr<string> path() const { return _path; }
    inline shared_ptr<string> query() const { return _query; }
    inline shared_ptr<string> header(const string &key) {
        if(_headers.find(key) == _headers.end())
            return shared_ptr<string>();
        return _headers.at(key);
    }
    inline void set_header(const string &key, shared_ptr<string> val) {
        _headers[key] = val;
    }
    inline void set_header(const string &key, const string &val) {
        _headers[key] = make_shared<string>(val);
    }
    const char *method_name();
    virtual int type() const;
    virtual ~http_request();

    virtual int serialize_size();
    virtual void serialize(char *buf);

    class decoder : public ::decoder {
    public:
        decoder();
        virtual bool decode(shared_ptr<streambuffer> &stb);
        virtual shared_ptr<message> msg();
        virtual ~decoder();
    private:
        shared_ptr<http_request> _msg;

        decoder(const decoder &);
        decoder &operator=(const decoder &);
    };
private:
    http_method _meth;
    shared_ptr<string> _resource;
    shared_ptr<string> _path;
    shared_ptr<string> _query;
    map<string, shared_ptr<string>> _headers;

    http_request &operator=(const http_request &);

    void set_method(const char *method);
};

class http_response : public message {
public:
    int code;

    inline shared_ptr<string> header(const string &key) {
        if(_headers.find(key) == _headers.end())
            return shared_ptr<string>();
        return _headers.at(key);
    }
    inline void set_header(const string &key, shared_ptr<string> val) {
        _headers[key] = val;
    }
    inline void set_header(const string &key, const string &val) {
        _headers[key] = make_shared<string>(val);
    }
    static const char *state_description(int code);

    virtual int serialize_size();
    virtual void serialize(char *buf);
    
    explicit http_response(int code);
    virtual int type() const;
    virtual ~http_response();

    class decoder : public ::decoder {
    public:
        decoder();
        virtual bool decode(shared_ptr<streambuffer> &stb);
        virtual shared_ptr<message> msg();
        virtual ~decoder();
    private:
        shared_ptr<http_response> _msg;

        decoder(const decoder &);
        decoder &operator=(const decoder &);
    };
private:
    map<string, shared_ptr<string>> _headers;

    http_response &operator=(const http_response &);
};

class http_connection;

class http_transaction {
public:
    http_transaction(shared_ptr<http_connection> conn,
                     shared_ptr<http_request> req);

    void serve_file(const string &filename, const string &mimetype);
    void forward_to(const string &hostname, int port);
    void display_error(int code);
    shared_ptr<http_response> make_response(int code);
    const shared_ptr<http_response> get_response();
    void flush_response();
    void write(const char *buf, int len);
    void write(const string &buf);

    inline bool header_sent() const {
        return _header_sent;
    }
    const shared_ptr<http_request> request;
    const shared_ptr<http_connection> connection;
    static const string SERVER_VERSION;
    shared_ptr<string> postdata;
private:
    bool _header_sent;
    shared_ptr<http_response> _response;
};

class http_service {
public:
    virtual void serve(shared_ptr<http_transaction> tx);
};

class http_connection {
public:
    http_connection(shared_ptr<http_service> svc,
                    shared_ptr<stream> strm,
                    shared_ptr<string> pname);
    shared_ptr<http_request> next_request();
    void invoke_service(shared_ptr<http_transaction> tx);
    bool keep_alive();
    inline shared_ptr<string> peername() {
        return _peername;
    }
private:
    bool _keep_alive;
    bool _upgraded;
    shared_ptr<stream> _strm;
    shared_ptr<string> _peername;
    shared_ptr<http_service> _svc;
    shared_ptr<http_request::decoder> _reqdec;
    friend class http_transaction;
};

class http_server {
public:
    shared_ptr<http_service> service;
    http_server(shared_ptr<http_service> svc);
    http_server(http_service *svc);
    virtual ~http_server();

    void listen(const char *addr, int port = 80);
private:
    uv_tcp_t *stream;

    http_server(const http_server &);
    http_server &operator=(const http_server &);
};

#endif
