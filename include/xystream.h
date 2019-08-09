#ifndef XYHTTPD_STREAM_H
#define XYHTTPD_STREAM_H

#include <uv.h>
#include <functional>

#include "xybuffer.h"
#include "xyfiber.h"

#define IOERR(r) RTERR("I/O Error: %s", uv_strerror(r))

class int_status : public wakeup_event {
public:
    inline int_status(int s) : _status(s) {};
    inline int_status(const int_status &s)
        : _status(s._status) {}
    static inline shared_ptr<int_status> make(int s) {
        return make_shared<int_status>(s);
    }
    inline int status() {
        return _status;
    }
    const char *strerror();
    virtual ~int_status();
private:
    int _status;
};

class stream : public enable_shared_from_this<stream> {
public:
    class write_request {
    public:
        write_request();

        union {
            uv_write_t write_req;
            uv_shutdown_t shutdown_req;
            uv_connect_t connect_req;
        };
        shared_ptr<fiber> _fiber;
    };

    shared_ptr<fiber> reading_fiber;

    class callbacks;
    friend class callbacks;

    virtual void accept(uv_stream_t *);
    template<class T>
    inline shared_ptr<T> read(const shared_ptr<decoder> &dec) {
        auto msg = read(dec);
        return msg ? dynamic_pointer_cast<T>(msg) : nullptr;
    }
    virtual shared_ptr<message> read(const shared_ptr<decoder> &dec);
    virtual void write(const char *buf, int length);
    virtual void pipe(const shared_ptr<stream> &sink);
    virtual bool has_tls();
    void write(const shared_ptr<message> &msg);
    void write(const string &str);
    void shutdown();
    void set_timeout(int timeout);
    virtual ~stream();
protected:
    shared_ptr<int_status> _do_read();
    virtual void _commit_rx(char *base, int nread);
    uv_stream_t *handle;
    uv_timer_t *_timeOuter;
    int _timeout;
    stream_buffer buffer;
    shared_ptr<decoder> _decoder;
    shared_ptr<stream> _pipe_src, _pipe_sink;
    stream();
private:
    stream(const stream &);
};

class ip_endpoint {
public:
    ip_endpoint(struct sockaddr_storage *_sa);
    ip_endpoint(const char *addr, int p);
    ip_endpoint(const string &addr, int p);
    int port();
    shared_ptr<string> straddr();
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
    virtual void connect(const string &host, int port);
    virtual void connect(shared_ptr<ip_endpoint> ep);
    void nodelay(bool enable);
    shared_ptr<ip_endpoint> getpeername();
private:
    virtual void connect(const sockaddr *sa);
};

class unix_stream : public stream {
public:
    unix_stream();
    virtual void connect(const shared_ptr<string> &path);
};

class tcp_server {
public:
    function<void(shared_ptr<tcp_stream>)> service_func;

    tcp_server(const char *addr, int port);
    virtual ~tcp_server();

    void serve(function<void(shared_ptr<tcp_stream>)> f);

    tcp_server(const tcp_server &) = delete;
    tcp_server &operator=(const tcp_server &) = delete;
protected:
    uv_tcp_t *_server;
};


#endif
