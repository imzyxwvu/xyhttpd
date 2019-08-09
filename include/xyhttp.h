#ifndef XYHTTPD_HTTP_H
#define XYHTTPD_HTTP_H

#include "xycommon.h"
#include "xyfiber.h"
#include "xystream.h"
#include "xyfcgi.h"

#include <unordered_map>
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
    void set_resource(shared_ptr<string> res);
    inline shared_ptr<string> path() const { return _path; }
    inline shared_ptr<string> query() const { return _query; }
    inline shared_ptr<string> header(const string &key) {
        auto it = _headers.find(key);
        return it == _headers.end() ? nullptr : it->second;
    }
    bool header_include(const string &key, const string &kw);
    inline void set_header(const string &key, shared_ptr<string> val) {
        _headers[key] = move(val);
    }
    inline void set_header(const string &key, const string &val) {
        _headers[key] = make_shared<string>(val);
    }
    void delete_header(const string &key);
    const char *method_name() const;
    virtual ~http_request();

    virtual int serialize_size();
    virtual void serialize(char *buf);

    unordered_map<string, shared_ptr<string>>::const_iterator hbegin() const;
    unordered_map<string, shared_ptr<string>>::const_iterator hend() const;

    class decoder : public ::decoder {
    public:
        decoder();
        virtual bool decode(stream_buffer &stb);
        virtual ~decoder();
    };
private:
    http_method _meth;
    shared_ptr<string> _resource;
    shared_ptr<string> _path;
    shared_ptr<string> _query;
    unordered_map<string, shared_ptr<string>> _headers;

    http_request &operator=(const http_request &);

    void set_method(const char *method);
};

class http_response : public message {
public:
    vector<shared_ptr<string>> cookies;

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
    virtual ~http_response();

    class decoder : public ::decoder {
    public:
        decoder();
        virtual bool decode(stream_buffer &stb);
        virtual ~decoder();
    };
private:
    int _code;
    unordered_map<string, shared_ptr<string>> _headers;

    http_response &operator=(const http_response &);
};

class http_transfer_decoder : public string_decoder {
public:
    http_transfer_decoder(shared_ptr<string> transferEnc);
    virtual bool decode(stream_buffer &stb);
private:
    bool _chunked;
};

class http_transaction {
public:
    http_transaction(shared_ptr<class http_connection> conn,
                     shared_ptr<http_request> req);
    ~http_transaction();

    void serve_file(const string &filename);
    void serve_file(const string &filename, struct stat &info);
    void forward_to(const string &hostname, int port);
    void forward_to(shared_ptr<ip_endpoint> ep);
    void forward_to(shared_ptr<fcgi_connection> conn);
    void redirect_to(const string &dest);
    void display_error(int code);
    shared_ptr<class websocket> accept_websocket();
    shared_ptr<http_response> get_response();
    shared_ptr<http_response> get_response(int code);
    shared_ptr<stream> upgrade(bool flush_resp = true);
    void write(const char *buf, int len);
    void transfer(const char *buf, int len);
    void write(const string &buf);
    void finish();

    inline bool header_sent() const { return _headerSent; }
    const shared_ptr<http_request> request;
    const shared_ptr<class http_connection> connection;
    static const string SERVER_VERSION;
    static const string WEBSOCKET_MAGIC;
    shared_ptr<string> postdata;
private:
    void flush_response();
    bool _headerSent, _finished;
    enum transfer_mode { UNDECIDED, SIMPLE, CHUNKED, UPGRADE, HEADONLY };
    transfer_mode _transfer_mode;
    struct z_stream_s *_gzip;
    stream_buffer _tx_buffer;
    shared_ptr<http_response> _response;
};

typedef const shared_ptr<http_transaction> http_trx;

class http_service {
public:
    virtual void serve(http_trx &tx) = 0;
    virtual ~http_service() = 0;
};

class http_connection {
public:
    http_connection(shared_ptr<http_service> svc,
                    shared_ptr<stream> strm,
                    shared_ptr<string> pname);
    virtual shared_ptr<http_request> next_request();
    virtual shared_ptr<stream> upgrade();
    virtual void invoke_service(http_trx &tx);
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
    explicit http_server(shared_ptr<http_service> svc);
    explicit http_server(http_service *svc);
    virtual ~http_server();

    virtual void start_thread(shared_ptr<stream> strm, shared_ptr<string> pname);

    void listen(const char *addr, int port);
    virtual void do_listen(int backlog);

    http_server(const http_server &) = delete;
    http_server &operator=(const http_server &) = delete;
protected:
    uv_tcp_t *_server;
};

#endif
