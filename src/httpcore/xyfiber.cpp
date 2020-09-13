#include <iostream>
#include <unistd.h>
#include <cstring>
#include <cstdarg>
#include "xyfiber.h"

using namespace std;

# include <sys/mman.h>
# include <execinfo.h>

std::string fmt(const char *f, ...) {
    va_list ap, ap2;
    va_start(ap, f);
    va_copy(ap2, ap);
    const int len = vsnprintf(NULL, 0, f, ap2);
    va_end(ap2);
    vector<char> zc(len + 1);
    vsnprintf(zc.data(), zc.size(), f, ap);
    va_end(ap);
    return string(zc.data(), len);
}

std::string timelabel() {
    time_t now = ::time(NULL);
    char tmlabel[32];
    int len = strftime(tmlabel, sizeof(tmlabel),
                       "%Y-%m-%d %H:%M:%S", ::localtime(&now));
    return string(tmlabel, len);
}

extended_runtime_error::extended_runtime_error
        (const char *fname, int lno, const string &wh) :
        _filename(fname), _lineno(lno), runtime_error(wh), _depth(0) {
    _depth = backtrace(_btbuf, 20) - 1;
}

const char *extended_runtime_error::filename() {
    return strrchr(_filename, '/') ?
           strrchr(_filename, '/') + 1 : _filename;
}

int extended_runtime_error::lineno() {
    return _lineno;
}

int extended_runtime_error::tracedepth() {
    return _depth;
}

char **extended_runtime_error::stacktrace() {
    return backtrace_symbols(_btbuf + 1, _depth);
}

ucontext_t fiber::_main_context;
fiber *fiber::_current;

fiber::fiber(std::function<void()> entry, size_t stack_size)
        : _stack_size(stack_size), _entry(std::move(entry)), _terminated(false), _prev(nullptr) {
    long pagesize = sysconf(_SC_PAGESIZE);
    if(_stack_size < 8 * pagesize || (_stack_size % pagesize) > 0)
        throw std::invalid_argument("stack size");
    _stack = (char *)mmap(nullptr, _stack_size, PROT_READ | PROT_WRITE,
                          MAP_PRIVATE | MAP_ANON, -1, 0);
    if(!_stack)
        throw std::bad_alloc();
    mprotect(_stack, pagesize, PROT_NONE);

    if(getcontext(&_context) == -1) {
        munmap(_stack, _stack_size);
        throw std::runtime_error(strerror(errno));
    }
    _context.uc_stack.ss_sp = _stack;
    _context.uc_stack.ss_size = _stack_size;
    _context.uc_link = nullptr;
    makecontext(&_context, reinterpret_cast<void (*)()>(fiber::_wrapper),1, this);
}

fiber::~fiber() {
    if(!_terminated) {
        std::cerr<<"fatal: non-terminated fiber destructor invoked"<<std::endl;
        abort();
    }
    munmap(_stack, _stack_size);
}

int fiber::yield(continuation &cont) {
    cont._pending = _current;
    swapcontext(&_current->_context,
                !_current->_prev ? &_main_context : &_current->_prev->_context);
    return cont._resume_status;
}

void fiber::resume() {
    if(_terminated || _current == this) {
        std::cerr<<"fatal: fiber not yielded (reference retained?)"<<std::endl;
        abort();
    }
    _prev = _current;
    _current = this;
    swapcontext(_prev ? &_prev->_context : &_main_context, &_context);

    fiber *previous_current = _prev; // backup member locally
    if(_terminated)
        delete this; // commit suicide (`this` invalidated)
    _current = previous_current;
}

void fiber::_wrapper(fiber *self) {
    try {
        self->_entry();
    }
    catch(std::exception &ex) {
        std::cerr<<timelabel()<<" terminated: "<<ex.what()<<std::endl;
    }
    self->_terminated = true;
    swapcontext(&self->_context,
                !self->_prev ? &_main_context : &self->_prev->_context);
}

void fiber::launch(std::function<void ()> entry, size_t stack_size) {
    auto *f = new fiber(std::move(entry), stack_size);
    f->resume();
}

continuation::continuation() : _pending(nullptr), _resume_status(0) {}

void continuation::resume(int status) {
    fiber *pending = _pending;
    if(!pending) {
        throw std::runtime_error("no yielded fiber");
    }
    _resume_status = status;
    _pending = nullptr;
    pending->resume();
}
