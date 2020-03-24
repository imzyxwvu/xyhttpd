#ifndef XYHTTPD_COMMON_H
#define XYHTTPD_COMMON_H

#include <memory>
#include <exception>
#include <cstring>

#define XY_PAGESIZE        8192

template<typename T>
using P = std::shared_ptr<T>;

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

class extended_runtime_error : public std::runtime_error {
public:
    extended_runtime_error(const char *fname, int lno, const std::string &wh);

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

class chunk {
private:
    struct _storage {
        size_t _size;
        int _nRef;
        char Y[0];
    };

    _storage *_X;

    inline void reset(_storage *X = nullptr) noexcept {
        if(_X && --_X->_nRef == 0)
            free(_X);
        _X = X;
    }
public:
    inline chunk() : _X(nullptr) {}
    chunk(const char *buf, size_t siz);
    inline chunk(const std::string &str) : chunk(str.data(), str.size()) {}
    inline chunk(const char *buf) : chunk(buf, buf ? strlen(buf) : 0) {}
    inline ~chunk() noexcept { reset(); }
    // Copy constructor and assignment should increase reference count (nRef++)
    inline chunk(const chunk &rhs) noexcept : _X(rhs._X) { if(_X) _X->_nRef++; }
    inline chunk &operator=(const chunk &rhs) noexcept {
        reset(rhs._X);
        if(_X) _X->_nRef++;
        return *this;
    }
    // Move constructors silently clear original reference
    inline chunk(chunk &&rhs) noexcept : _X(rhs._X) { rhs._X = nullptr; }
    inline chunk &operator=(chunk &&chunk) noexcept {
        reset(chunk._X);
        chunk._X = nullptr;
        return *this;
    }

    inline chunk &operator=(const std::string &str) {
        *this = std::move(chunk(str));
    }

    inline chunk &operator=(const char *str) {
        if(str) {
            *this = chunk(str);
        } else {
            reset();
        }
        return *this;
    }

    inline long find(const char *_S) {
        const char *_P = strstr(_X->Y, _S);
        if(_P) return _P - _X->Y;
        return -1;
    }

    inline const char *data() const noexcept {
        return _X ? _X->Y : nullptr;
    }

    inline std::string substr(int skip) const {
        if(!_X || skip > _X->_size)
            throw std::out_of_range("chunk::substr invalid skip");
        return std::string(_X->Y + skip, _X->_size - skip);
    }

    inline bool operator==(const chunk &rhs) noexcept {
        if(_X == rhs._X) return true;
        if(_X == nullptr || rhs._X == nullptr) return false;
        if(size() != rhs.size()) return false;
        return memcmp(data(), rhs.data(), size()) == 0;
    }

    inline std::string to_string() { return std::string(data(), size()); }
    inline size_t size() const noexcept { return _X ? _X->_size : 0; }
    inline char &operator[] (int idx) { return _X->Y[idx]; }
    inline operator bool() const noexcept { return (bool)_X; }
    inline bool empty() const noexcept { return _X ? _X->_size == 0 : true; }
};

inline bool operator==(const std::string &_S, const chunk &_C) {
    if(!_C || _S.size() != _C.size()) return false;
    return memcmp(_S.data(), _C.data(), _S.size()) == 0;
}

#endif