#ifndef XYHTTPD_HTTP_H
#define XYHTTPD_HTTP_H

#include "xycommon.h"
#include "xyfiber.h"
#include "xystream.h"
#include "xyfcgi.h"

#include <unordered_map>
#include <uv.h>
#include <vector>

class http_request : public message {
public:
    std::string method;
    http_request() = default;
    inline chunk resource() const { return _resource; }
    void set_resource(chunk res);
    inline const std::string &path() const { return _path; }
    inline chunk query() const { return _query; }
    inline chunk header(const std::string &key) {
        auto it = _headers.find(key);
        return it == _headers.end() ? nullptr : it->second;
    }
    bool header_include(const std::string &key, const std::string &kw);
    inline void set_header(const std::string &key, chunk val) {
        _headers[key] = std::move(val);
    }
    void delete_header(const std::string &key);
    virtual ~http_request();

    virtual int serialize_size();
    virtual void serialize(char *buf);

    std::unordered_map<std::string, chunk>::const_iterator hbegin() const;
    std::unordered_map<std::string, chunk>::const_iterator hend() const;

    class decoder : public ::decoder {
    public:
        virtual bool decode(stream_buffer &stb);
        virtual ~decoder();
    };

    static std::string url_decode(const char *buf, int siz);
private:
    chunk _resource, _query;
    std::string _path;
    std::unordered_map<std::string, chunk> _headers;

    http_request &operator=(const http_request &);
};

class http_response : public message {
public:
    std::vector<chunk> cookies;

    inline chunk header(const std::string &key) {
        auto it = _headers.find(key);
        return it == _headers.end() ? nullptr : it->second;
    }
    inline int code() {
        return _code;
    }
    void set_code(int newcode);
    void set_header(const std::string &key, chunk val);
    void delete_header(const std::string &key);
    static const char *state_description(int code);

    virtual int serialize_size();
    virtual void serialize(char *buf);
    
    explicit http_response(int code);
    virtual ~http_response();

    class decoder : public ::decoder {
    public:
        virtual bool decode(stream_buffer &stb);
        virtual ~decoder();
    };
private:
    int _code;
    std::unordered_map<std::string, chunk> _headers;

    http_response &operator=(const http_response &);
};

class http_transfer_decoder : public string_decoder {
public:
    explicit http_transfer_decoder(const P<http_response> &resp);
    virtual bool decode(stream_buffer &stb);
private:
    bool _chunked;
};

class http_transaction {
public:
    const P<http_request> request;
    chunk postdata;
    const P<class http_connection> connection;

    http_transaction(P<class http_connection> conn,
                     P<http_request> req);
    ~http_transaction();

    void serve_file(const std::string &filename);
    void serve_file(const std::string &filename, struct stat &info);
    void forward_to(const std::string &hostname, int port);
    void forward_to(P<stream> strm);
    void forward_to(P<fcgi_connection> conn);
    void redirect_to(const std::string &dest);
    void display_error(int code);
    P<class websocket> accept_websocket();
    P<http_response> get_response();
    P<http_response> get_response(int code);
    P<stream> upgrade(bool flush_resp = true);

    inline bool header_sent() const { return _headerSent; }
    void write(const char *buf, int len);
    void write(const std::string &buf);
    void finish();

    static const std::string SERVER_VERSION;
    static const std::string WEBSOCKET_MAGIC;
    enum transfer_mode { UNDECIDED, SIMPLE, CHUNKED, UPGRADE, HEADONLY };
private:
    bool _headerSent, _finished, _noGzip;
    transfer_mode _transfer_mode;
    struct z_stream_s *_gzip;
    stream_buffer _tx_buffer;
    P<http_response> _response;

    void start_transfer(transfer_mode mode);
    void transfer(const char *buf, int len);
};

using http_trx = const P<http_transaction>;

class http_service {
public:
    virtual void serve(http_trx &tx) = 0;
    virtual ~http_service() = 0;
};

class http_connection {
public:
    http_connection(P<stream> strm, std::string pname);
    virtual P<http_request> next_request();
    virtual P<stream> upgrade();
    virtual void invoke_service(const P<http_service> &svc, http_trx &tx);
    virtual bool keep_alive();
    inline std::string peername() { return _peername; }
    bool has_tls();
private:
    bool _keep_alive, _upgraded;
    P<stream> _strm;
    std::string _peername;
    P<http_request::decoder> _reqdec;
    friend class http_transaction;
};

class http_server {
public:
    P<http_service> service;
    explicit http_server(P<http_service> svc);
    explicit http_server(http_service *svc);
    virtual ~http_server();

    virtual void service_loop(P<http_connection> conn);
    void listen(const char *addr, int port);
    virtual void do_listen(int backlog);

    http_server(const http_server &) = delete;
    http_server &operator=(const http_server &) = delete;
protected:
    uv_tcp_t *_server;
};

class websocket_frame : public message {
public:
    class decoder : public ::decoder {
    public:
        explicit decoder(int maxPayloadLen);
        virtual bool decode(stream_buffer &stb);
    private:
        int _max_payload;
    };

    websocket_frame(int op, chunk payload);
    virtual ~websocket_frame();
    inline int opcode() { return _op & 0xf; }
    inline bool fin() { return (_op & 0x80) > 0; }
    inline bool deflated() { return (_op & 0x40) > 0; }
    inline chunk payload() { return _payload; }
    virtual int serialize_size();
    virtual void serialize(char *buf);

private:
    int _op;
    chunk _payload;
};

class websocket {
public:
    websocket(const P<stream> &strm, bool _deflate);
    bool poll();
    chunk read();
    virtual ~websocket();
    virtual void send(const chunk &msg);
    void send(P<websocket_frame> frame);

private:
    void cleanup();
    void flush_writing();
    P<stream> _strm;
    stream_buffer _reassembled;
    chunk _done;
    bool _msg_deflated, _alive;
    struct z_stream_s *_tx_zs, *_rx_zs;
    P<websocket_frame::decoder> _decoder;
};

#endif
