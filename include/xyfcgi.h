#ifndef XYHTTPD_FCGI_H
#define XYHTTPD_FCGI_H

#include "xycommon.h"
#include "xyfiber.h"
#include "xystream.h"

#include <unordered_map>
#include <uv.h>

class fcgi_message : public message {
public:

    enum message_type {
        FCGI_BEGIN_REQUEST = 1, FCGI_ABORT_REQUEST,
        FCGI_END_REQUEST, FCGI_PARAMS, FCGI_STDIN,
        FCGI_STDOUT, FCGI_STDERR };

    fcgi_message(message_type t, int requestId);
    fcgi_message(message_type t, int requestId, const char *data, int len);
    ~fcgi_message();

    inline message_type msgtype() const {
        return _type;
    }
    inline char *data() {
        return _payload;
    }
    inline int request_id() {
        return _request_id;
    }
    inline int length() const {
        return _payload_length;
    }
    virtual void serialize(char *buf);
    virtual int serialize_size();
    static shared_ptr<fcgi_message> make_dummy(message_type t);

    class decoder : public ::decoder {
    public:
        decoder() = default;
        virtual bool decode(stream_buffer &stb);
        virtual ~decoder();
    };
private:
    message_type _type;
    char *_payload;
    int _payload_length;
    int _request_id;
};

class fcgi_connection {
public:
    void set_env(const string &key, shared_ptr<string> val);
    void set_env(const string &key, const string &val);
    shared_ptr<string> get_env(const string &key);
    void write(const char *data, int len);
    void write(const shared_ptr<string> &msg);
    template<class T>
    inline shared_ptr<T> read(shared_ptr<decoder> dec) {
        return dynamic_pointer_cast<T>(read(move(dec)));
    }
    shared_ptr<message> read(shared_ptr<decoder> dec);

    fcgi_connection(const shared_ptr<stream> &strm, int roleId);
private:
    fcgi_connection(const fcgi_connection &);

    void flush_env();
    unordered_map<string, shared_ptr<string>> _env;
    shared_ptr<stream> _strm;
    bool _envready;
    stream_buffer _buffer;
};

class fcgi_provider { 
public:
    virtual shared_ptr<fcgi_connection> get_connection() = 0;
};

class tcp_fcgi_provider : public fcgi_provider { 
public:
    virtual shared_ptr<fcgi_connection> get_connection();
    tcp_fcgi_provider(const string &hostip, int port);
private:
    string _hostip;
    int _port;
};

class unix_fcgi_provider : public fcgi_provider {
public:
    virtual shared_ptr<fcgi_connection> get_connection();
    unix_fcgi_provider(const string &path);
    unix_fcgi_provider(const shared_ptr<string> &path);
    inline shared_ptr<string> path() { return _path; }
private:
    shared_ptr<string> _path;
};

#endif