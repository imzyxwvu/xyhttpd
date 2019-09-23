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
    static P<fcgi_message> make_dummy(message_type t);

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
    void set_env(const std::string &key, chunk val);
    chunk get_env(const std::string &key);
    void write(const char *data, int len);
    void write(const chunk &msg);
    template<class T>
    inline P<T> read(P<decoder> dec) {
        return std::dynamic_pointer_cast<T>(read(move(dec)));
    }
    P<message> read(P<decoder> dec);

    fcgi_connection(const P<stream> &strm, int roleId);
private:
    fcgi_connection(const fcgi_connection &);

    void flush_env();
    std::unordered_map<std::string, chunk> _env;
    P<stream> _strm;
    bool _envready;
    stream_buffer _buffer;
};

class fcgi_provider { 
public:
    virtual P<fcgi_connection> get_connection() = 0;
};

class tcp_fcgi_provider : public fcgi_provider { 
public:
    virtual P<fcgi_connection> get_connection();
    tcp_fcgi_provider(const std::string &hostip, int port);
private:
    std::string _hostip;
    int _port;
};

class unix_fcgi_provider : public fcgi_provider {
public:
    virtual P<fcgi_connection> get_connection();
    unix_fcgi_provider(const std::string &path);
    unix_fcgi_provider(const P<std::string> &path);
    inline const std::string &path() const { return _path; }
private:
    std::string _path;
};

#endif