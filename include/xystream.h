#ifndef XYHTTPD_STREAM_H
#define XYHTTPD_STREAM_H

#include <uv.h>
#include <functional>

#include "xyfiber.h"

#define IOERR(r) RTERR("I/O Error: %s", uv_strerror(r))

class stream_buffer {
public:
    stream_buffer();
    void pull(int nbytes);
    char *prepare(size_t nbytes);
    void commit(size_t nbytes);
    void append(const P<message> &msg);
    void append(const void *p, int nbytes);
    char *detach();
    inline char operator[](int i) { return _data[i]; }
    inline size_t size() const { return _avail; }
    inline char *data() const { return (char *)_data; };
    inline chunk dump() const { return chunk(_data, _avail); }
    virtual ~stream_buffer();
private:
    char *_data;
    size_t _avail;

    stream_buffer(const stream_buffer &);
    stream_buffer &operator=(const stream_buffer &);
};

class decoder {
public:
    decoder() = default;
    decoder(const decoder &) = delete;
    virtual bool decode(stream_buffer &stb) = 0;
    inline P<message> msg() { return std::move(_msg); }
    virtual ~decoder() = 0;
protected:
    P<message> _msg;
};

class string_message : public message {
public:
    string_message(const char *buf, size_t len);
    inline chunk &str() { return _data; }
    inline const char *data() { return _data.data(); }
    virtual ~string_message();

    virtual int serialize_size();
    virtual void serialize(char *buf);
private:
    chunk _data;
};

class string_decoder : public decoder {
public:
    explicit string_decoder(int bytesToRead = -1);
    virtual bool decode(stream_buffer &stb);
    inline bool more() { return _restBytes != 0; }
    virtual ~string_decoder();
protected:
    int _nBytes, _restBytes;
};

class stream : public std::enable_shared_from_this<stream> {
public:
    class write_request {
    public:
        write_request();

        union {
            uv_write_t write_req;
            uv_shutdown_t shutdown_req;
            uv_connect_t connect_req;
        };
        continuation _cont;
    };

    continuation read_cont;

    class callbacks;
    friend class callbacks;

    virtual void accept(uv_stream_t *);
    virtual void read(const P<decoder> &dec);
    virtual void write(const char *buf, int length);
    virtual bool has_tls();
    void write(const P<message> &msg);
    void write(const chunk &str);
    void shutdown();
    void set_timeout(int timeout);
    virtual ~stream();

    template<class T>
    inline P<T> read(const P<decoder> &dec) {
        (void)read(dec);
        return std::dynamic_pointer_cast<T>(dec->msg());
    }
protected:
    int _do_read();
    virtual void _commit_rx(char *base, int nread);
    uv_stream_t *handle;
    uv_timer_t *_timeOuter;
    int _timeout;
    stream_buffer buffer;
    P<decoder> _decoder;
    stream();
private:
    stream(const stream &);
};

class ip_endpoint {
public:
    ip_endpoint(struct sockaddr_storage *_sa);
    ip_endpoint(const char *addr, int p);
    ip_endpoint(const std::string &addr, int p);
    int port();
    std::string straddr();
    const sockaddr *sa();
private:
    union {
        struct sockaddr_storage _sa;
        struct sockaddr_in _sa_in;
        struct sockaddr_in6 _sa_in6;
    };
};

class tcp_stream : public stream {
public:
    tcp_stream();
    virtual void connect(const std::string &host, int port);
    virtual void connect(P<ip_endpoint> ep);
    void nodelay(bool enable);
    P<ip_endpoint> getpeername();
private:
    virtual void connect(const sockaddr *sa);
};

class unix_stream : public stream {
public:
    unix_stream();
    virtual void connect(const std::string &path);
};

class tcp_server {
public:
    std::function<void(P<tcp_stream>)> service_func;

    tcp_server(const char *addr, int port);
    virtual ~tcp_server();

    void serve(std::function<void(P<tcp_stream>)> f);

    tcp_server(const tcp_server &) = delete;
    tcp_server &operator=(const tcp_server &) = delete;
protected:
    uv_tcp_t *_server;
};


#endif
