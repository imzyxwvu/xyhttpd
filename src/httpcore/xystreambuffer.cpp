#include "xycommon.h"
#include <cstring>
#include <vector>

void message::serialize(char *buf) {
    throw RTERR("serialize not implemented");
}

message::~message() {}

decoder::~decoder() {}

void streambuffer::pull(int nbytes) {
    if(nbytes < _size) {
        memmove(_data, _data + nbytes, _size - nbytes);
        _size -= nbytes;
        _data = (char *)realloc(_data, _size);
    } else {
        _size = 0;
        free(_data);
        _data = NULL;
    }
}

void streambuffer::enlarge(int nbytes) {
    _size += nbytes;
}

void streambuffer::append(const void *buffer, int nbytes) {
    memcpy(prepare(nbytes), buffer, nbytes);
    enlarge(nbytes);
}

char *streambuffer::prepare(int nbytes) {
    char *p = (char *)realloc(_data, _size + nbytes);
    if(p) {
        _data = p;
        return _data + _size;
    } else {
        return NULL;
    }
}

char *streambuffer::detach() {
    char *_old = _data;
    _data = nullptr;
    _size = 0;
    return _old;
}

streambuffer::~streambuffer() {
    free(_data);
    _size = 0;
}

streambuffer::streambuffer() : _data(NULL), _size(0) {}