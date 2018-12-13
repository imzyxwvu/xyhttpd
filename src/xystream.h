#ifndef XYHTTPD_STREAM_H
#define XYHTTPD_STREAM_H

#include <uv.h>

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

class stream {
public:
    shared_ptr<streambuffer> buffer;
    shared_ptr<fiber> reading_fiber, writing_fiber;

    virtual int accept(uv_stream_t *);
    template<class T>
    inline shared_ptr<T> read(shared_ptr<decoder> dec) {
        return dynamic_pointer_cast<T>(read(dec));
    }
    virtual shared_ptr<message> read(shared_ptr<decoder>);
    virtual void write(const char *buf, int length);
    void write(shared_ptr<message> msg);
    void write(const string &str);
    virtual ~stream();
protected:
    uv_stream_t *handle;
    stream();
private:
    uv_write_t _wreq;
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
    void connect(const string &host, int port);
    shared_ptr<ip_endpoint> getpeername();
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
private:
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