#ifndef XYHTTPD_HTTP_H
#define XYHTTPD_HTTP_H

#include "xycommon.h"
#include "xyfiber.h"
#include "xystream.h"
#include "xyfcgi.h"

#include <map>
#include <uv.h>
#include <vector>

enum http_method {
    GET, HEAD, POST, PUT, DELETE, OPTIONS,
    CONNECT, // HTTP Tunnelling
    BREW, // Hypertext Coffee Pot Control Protocol
    M_SEARCH, // Universal PnP Multicast Search
};

class http_request : public message {
public:
    http_request();
    inline http_method method() const { return _meth; };
    inline shared_ptr<string> resource() const { return _resource; }
    inline shared_ptr<string> path() const { return _path; }
    inline shared_ptr<string> query() const { return _query; }
    inline shared_ptr<string> header(const string &key) {
        auto it = _headers.find(key);
        return it == _headers.end() ? nullptr : it->second;
    }
    bool header_include(const string &key, const string &kw);
    inline void set_header(const string &key, shared_ptr<string> val) {
        _headers[key] = val;
    }
    inline void set_header(const string &key, const string &val) {
        _headers[key] = make_shared<string>(val);
    }
    void delete_header(const string &key);
    const char *method_name() const;
    virtual int type() const;
    virtual ~http_request();

    virtual int serialize_size();
    virtual void serialize(char *buf);

    map<string, shared_ptr<string>>::const_iterator hbegin() const;
    map<string, shared_ptr<string>>::const_iterator hend() const;

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
    inline shared_ptr<string> header(const string &key) {
        auto it = _headers.find(key);
        return it == _headers.end() ? nullptr : it->second;
    }
    inline int code() {
        return _code;
    }
    void set_code(int newcode);
    void set_header(const string &key, const string &val);
    void set_header(const string &key, shared_ptr<string> val);
    void delete_header(const string &key);
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
    int _code;
    map<string, shared_ptr<string>> _headers;
    vector<shared_ptr<string>> _cookies;

    http_response &operator=(const http_response &);
};

class http_transfer_decoder : public string_decoder {
public:
    http_transfer_decoder(shared_ptr<string> transferEnc);
    virtual bool decode(shared_ptr<streambuffer> &stb);
private:
    bool _chunked;
};

class http_connection;

class http_transaction {
public:
    http_transaction(shared_ptr<http_connection> conn,
                     shared_ptr<http_request> req);

    void serve_file(const string &filename);
    void forward_to(const string &hostname, int port);
    void forward_to(shared_ptr<ip_endpoint> ep);
    void forward_to(shared_ptr<fcgi_connection> conn);
    void redirect_to(const string &dest);
    void display_error(int code);
    void declare_length(int len);
    shared_ptr<class websocket> accept_websocket();
    shared_ptr<http_response> get_response();
    shared_ptr<http_response> get_response(int code);
    shared_ptr<stream> upgrade();
    void write(const char *buf, int len);
    void write(const string &buf);
    void finish();

    inline bool header_sent() const { return _headerSent; }
    const shared_ptr<http_request> request;
    const shared_ptr<http_connection> connection;
    static const string SERVER_VERSION;
    static const string WEBSOCKET_MAGIC;
    shared_ptr<string> postdata;
private:
    void flush_response();
    void write_chunk(const char *buf, int len);
    bool _headerSent, _finished;
    enum transfer_mode { UNDECIDED, SIMPLE, GZIP, CHUNKED, UPGRADE };
    transfer_mode _transfer_mode;
    streambuffer _tx_buffer;
    shared_ptr<http_response> _response;
};

class http_service {
public:
    virtual void serve(shared_ptr<http_transaction> tx) = 0;
    virtual ~http_service() = 0;
};

class http_connection {
public:
    http_connection(shared_ptr<http_service> svc,
                    shared_ptr<stream> strm,
                    shared_ptr<string> pname);
    virtual shared_ptr<http_request> next_request();
    virtual shared_ptr<stream> upgrade();
    virtual void invoke_service(shared_ptr<http_transaction> tx);
    virtual bool keep_alive();
    inline shared_ptr<string> peername() {
        return _peername;
    }
    bool has_tls();
private:
    bool _keep_alive, _upgraded;
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

    virtual void start_thread(shared_ptr<stream> strm, shared_ptr<string> pname);

    void listen(const char *addr, int port);
    virtual void handle_incoming();
protected:
    int _fd;
    uv_poll_t _poller;

    http_server(const http_server &);
    http_server &operator=(const http_server &);
};

#endif
