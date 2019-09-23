#ifndef XYHTTPD_XYBUFFER_H
#define XYHTTPD_XYBUFFER_H

#include "xycommon.h"

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
    inline P<message> msg() { return _msg; }
    virtual ~decoder() = 0;
protected:
    P<message> _msg;
};

class string_message : public message {
public:
    string_message(char *buf, size_t len);
    explicit string_message(const std::string &str);
    inline chunk str() { return chunk(_buffer, _size); }
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
    explicit string_decoder(int bytesToRead = -1);
    virtual bool decode(stream_buffer &stb);
    inline bool more() { return _restBytes != 0; }
    virtual ~string_decoder();
protected:
    int _nBytes, _restBytes;
};

#endif //XYHTTPD_XYBUFFER_H
