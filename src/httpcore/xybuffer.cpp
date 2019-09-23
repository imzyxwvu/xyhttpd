#include "xybuffer.h"
#include <cstring>
#include <vector>

using namespace std;

chunk::chunk(const char *buf, size_t siz) : _X(nullptr) {
    if(buf == nullptr)
        return;
    _X = (_storage *)malloc(sizeof(_storage) + siz + 1);
    if(!_X)
        throw std::bad_alloc();
    _X->_nRef = 1;
    _X->_size = siz;
    memcpy(_X->Y, buf, siz);
    _X->Y[siz] = 0;
}

void message::serialize(char *buf) {
    throw RTERR("serialize not implemented");
}

message::~message() {}

decoder::~decoder() {}

stream_buffer::stream_buffer() : _data(NULL), _avail(0) {}

/**
 * Remove n bytes of proceeded data remove the beginning of buffer.
 * @param nbytes Byte count to remove.
 */
void stream_buffer::pull(int nbytes) {
    if(nbytes < _avail) {
        memmove(_data, _data + nbytes, _avail - nbytes);
        _avail -= nbytes;
        _data = (char *)realloc(_data, _avail);
    } else {
        _avail = 0;
        free(_data);
        _data = NULL;
    }
}

/**
 * Commit n bytes of new data at the prepared area of buffer.
 * @param nbytes Byte count to commit.
 */
void stream_buffer::commit(size_t nbytes) {
    _avail += nbytes;
    // Shrink the buffer. If returned address is not _data, logic
    // error or memory overrun must occurred. Check it!
    _data = (char *)realloc(_data, _avail);
}

/**
 * Serial message in msg and append the serialized data to the buffer.
 * @param msg Pointer to the message.
 */
void stream_buffer::append(const P<message> &msg) {
    int size = msg->serialize_size();
    if(size > 0) {
        msg->serialize(prepare(size));
        commit(size);
    }
}

/**
 * Append n bytes of data at address p to the end of buffer.
 * @param data Address to new data.
 * @param nbytes Byte count.
 */
void stream_buffer::append(const void *p, int nbytes) {
    memcpy(prepare(nbytes), p, nbytes);
    commit(nbytes);
}

/**
 * Preallocate n bytes of memory at the end of buffer.
 * If there was pre-allocated area, previous data is preserved.
 * @param nbytes Byte count to preallocate.
 * @return The address to the beginning of pre-allocated area.
 */
char *stream_buffer::prepare(size_t nbytes) {
    char *p = (char *)realloc(_data, _avail + nbytes);
    if(!p) throw std::bad_alloc();
    _data = p;
    return _data + _avail;
}

/**
 * Detach all data in buffer. Ownership of the buffer is transferred,
 * thus you should free the returned buffer later.
 * @return The beginning address of the buffer.
 */
char *stream_buffer::detach() {
    char *_old = _data;
    _data = nullptr;
    _avail = 0;
    return _old;
}

stream_buffer::~stream_buffer() {
    free(_data);
    _avail = 0;
}

string_message::string_message(const string &s) : _size(s.size()) {
    _buffer = (char *)malloc(_size);
    if(!_buffer)
        throw std::bad_alloc();
    memcpy(_buffer, s.data(), _size);
}

string_message::string_message(char *buf, size_t len)
        : _buffer(buf), _size(len) {}

int string_message::serialize_size() {
    return _size;
}

void string_message::serialize(char *buf) {
    memcpy(buf, _buffer, _size);
}

string_message::~string_message() {
    free(_buffer);
    _size = 0;
}

string_decoder::string_decoder(int bytesToRead)
    : _nBytes(0), _restBytes(bytesToRead) {}

bool string_decoder::decode(stream_buffer &stb) {
    if(stb.size() == 0)
        return false;
    _nBytes = stb.size();
    if(_restBytes == -1) {
        _msg = make_shared<string_message>(stb.detach(), _nBytes);
        return true;
    } else if(stb.size() <= _restBytes) {
        _msg = make_shared<string_message>(stb.detach(), _nBytes);
    } else {
        _nBytes = _restBytes;
        char *buffer = (char *)malloc(_nBytes);
        memcpy(buffer, stb.data(), _nBytes);
        _msg = make_shared<string_message>(buffer, _nBytes);
        stb.pull(_nBytes);
    }
    _restBytes -= _nBytes;
    return true;
}

string_decoder::~string_decoder() {}
