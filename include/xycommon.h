#ifndef XYHTTPD_COMMON_H
#define XYHTTPD_COMMON_H

#include <memory>
#include <exception>

using namespace std;

#define XY_PAGESIZE        8192

class message {
public:
    virtual ~message() = 0;

    virtual int serialize_size() = 0;
    virtual void serialize(char *buf);
};


std::string timelabel();
std::string fmt(const char * f, ...);
std::string base64_encode(unsigned char const* , unsigned int len);
std::string base64_decode(std::string const& s);
template<typename T> inline T *mem_alloc() {
    T* p = (T*)malloc(sizeof(T));
    if(NULL == p) throw std::bad_alloc();
    return p;
}

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