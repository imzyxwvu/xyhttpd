#ifndef XYHTTPD_STREAM_H
#define XYHTTPD_STREAM_H

#include <uv.h>
#include <queue>

#include "xycommon.h"
#include "xyfiber.h"

#define IOERR(r) RTERR("I/O Error: %s", uv_strerror(r))

class int_status : public wakeup_event {
public:
    inline int_status(int s) : _status(s) {};
    inline int_status(const int_status &s)
        : _status(s._status) {}
    static inline shared_ptr<int_status> make(int s) {
        return shared_ptr<int_status>(new int_status(s));
    }
    inline int status() {
        return _status;
    }
    const char *strerror();
    const char *errname();
    virtual ~int_status();
private:
    int _status;
};

class write_request {
public:
    write_request(char *data, size_t len);
    write_request(const string &data);
    write_request(shared_ptr<message> data);
    write_request(const write_request &) = delete;
    ~write_request();

    inline const char *base() {
        return _data + _cur;
    }

    inline size_t size() {
        return _len - _cur;
    }

    void associate(shared_ptr<fiber> _bound_fiber);
    void abort(int eno);
    bool confirm(int len);
private:
    char *_data;
    size_t _len, _cur;
    shared_ptr<fiber> _bound_fiber;
};

class stream {
public:
    shared_ptr<streambuffer> buffer;

    template<class T>
    inline shared_ptr<T> read(shared_ptr<decoder> dec) {
        auto msg = read(dec);
        return msg ? dynamic_pointer_cast<T>(msg) : nullptr;
    }
    virtual shared_ptr<message> read(shared_ptr<decoder>);
    virtual void write(const char *buf, int length);
    virtual bool has_tls();
    void write(shared_ptr<message> msg);
    void write(const string &str);
    void dispatch_event(int events);
    virtual ~stream();
protected:
    queue<shared_ptr<write_request>> _write_queue;
    shared_ptr<fiber> _reading_fiber;
    bool _connecting;
    uv_poll_t _poller;
    int _fd;
    bool _readable, _eof;
    shared_ptr<decoder> _reading_decoder;
    stream(int fd);
    stream(const stream &) = delete;
    void connect(const sockaddr *addr, int len);
    int fill_read_buffer(shared_ptr<decoder> tryDecoder);
    void do_write_request(shared_ptr<write_request> req);
    void flush_write_queue();
    const int READ_STEP = 0x1000;
};

class ip_endpoint {
public:
    ip_endpoint(struct sockaddr_storage *_sa);
    ip_endpoint(const char *addr, int p);
    ip_endpoint(const string &addr, int p);
    int port();
    shared_ptr<string> straddr();
    const sockaddr *sa();
    const size_t size();
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
    shared_ptr<ip_endpoint> getpeername();
    static shared_ptr<tcp_stream> accept(int fd);
protected:
    tcp_stream(int fd, shared_ptr<ip_endpoint> ep);
    shared_ptr<ip_endpoint> _remote_ep;
};

class unix_stream : public stream {
public:
    unix_stream();
    virtual void connect(const shared_ptr<string> path);
};

class string_message : public message {
public:
    string_message(const char *buf, int len);
    explicit string_message(const string &str);
    inline shared_ptr<string> str() {
        return make_shared<string>(_str);
    }
    inline const char *data() { return _str.data(); }
    virtual int type() const;
    virtual ~string_message();

    virtual int serialize_size();
    virtual void serialize(char *buf);
private:
    string _str;
};

class string_decoder : public decoder {
public:
    string_decoder();
    virtual bool decode(shared_ptr<streambuffer> &stb);
    virtual shared_ptr<message> msg();
    virtual ~string_decoder();
protected:
    int nbyte;
    char *buffer;
};

class rest_decoder : public decoder {
public:
    rest_decoder(int rest);
    virtual bool decode(shared_ptr<streambuffer> &stb);
    virtual shared_ptr<message> msg();
    virtual ~rest_decoder();
    inline bool more() { return nrest > 0; }
private:
    int nbyte, nrest;
    char *buffer;
};

#endif
