#ifndef XYHTTPD_COMMON_H
#define XYHTTPD_COMMON_H

#include <memory>
#include <exception>

using namespace std;

#define XY_MESSAGE_REQUEST 1
#define XY_MESSAGE_STRING  2
#define XY_MESSAGE_CHUNK   3
#define XY_MESSAGE_WSFRAME 4
#define XY_MESSAGE_FCGI    5
#define XY_MESSAGE_RESP    6

class message {
public:
    virtual int type() const = 0;
    virtual ~message() = 0;

    virtual int serialize_size() = 0;
    virtual void serialize(char *buf);
};

class streambuffer {
public:
    streambuffer();
    virtual void pull(int nbytes);
    virtual void *prepare(int nbytes);
    virtual void enlarge(int nbytes);
    virtual void append(void *buffer, int nbytes);
    inline int size() const { return _size; }
    inline char *data() const { return (char *)_data; };
    virtual ~streambuffer();
    static shared_ptr<streambuffer> alloc();
private:
    char *_data;
    int _size;

    streambuffer(const streambuffer &);
    streambuffer &operator=(const streambuffer &);
};

class decoder {
public:
    virtual bool decode(shared_ptr<streambuffer> &stb) = 0;
    virtual shared_ptr<message> msg() = 0;
    virtual ~decoder() = 0;
};

std::string timelabel();
std::string fmt(const char * const f, ...);
std::string base64_encode(unsigned char const* , unsigned int len);
std::string base64_decode(std::string const& s);

class extended_runtime_error : public runtime_error {
public:
    extended_runtime_error(const char *fname, int lno, const string &wh);

    const char *filename();
    int lineno();
    int tracedepth();
    char **stacktrace();
private:
    const char *_filename;
    int _lineno, _depth;
    void *_btbuf[20];
};

#define RTERR(...) \
    extended_runtime_error(__FILE__, __LINE__, fmt(__VA_ARGS__))

#endif