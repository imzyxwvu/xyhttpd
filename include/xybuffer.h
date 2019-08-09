#ifndef XYHTTPD_XYBUFFER_H
#define XYHTTPD_XYBUFFER_H

#include "xycommon.h"

class stream_buffer {
public:
    stream_buffer();
    void pull(int nbytes);
    char *prepare(size_t nbytes);
    void commit(size_t nbytes);
    void append(const shared_ptr<message> &msg);
    void append(const void *p, int nbytes);
    char *detach();
    inline char operator[](int i) { return _data[i]; }
    inline size_t size() const { return _avail; }
    inline char *data() const { return (char *)_data; };
    inline shared_ptr<string> to_string() const {
        return make_shared<string>(_data, _avail);
    }
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
    inline shared_ptr<message> msg() { return _msg; }
    virtual ~decoder() = 0;
protected:
    shared_ptr<message> _msg;
};

class string_message : public message {
public:
    string_message(char *buf, size_t len);
    explicit string_message(const string &str);
    inline shared_ptr<string> str() {
        return make_shared<string>(_buffer, _size);
    }
    inline const char *data() { return _buffer; }
    virtual ~string_message();

    virtual int serialize_size();
    virtual void serialize(char *buf);
private:
    char *_buffer;
    size_t _size;
};

class string_decoder : public decoder {
public:
    string_decoder();
    virtual bool decode(stream_buffer &stb);
    virtual ~string_decoder();
protected:
    int nbyte;
};

class rest_decoder : public string_decoder {
public:
    rest_decoder(int rest);
    virtual bool decode(stream_buffer &stb);
    inline bool more() { return nrest > 0; }
    virtual ~rest_decoder();
private:
    int nrest;
};

#endif //XYHTTPD_XYBUFFER_H
