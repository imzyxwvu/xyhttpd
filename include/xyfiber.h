#ifndef XYHTTPD_FIBER_H
#define XYHTTPD_FIBER_H

#ifdef _WIN32
typedef void *fiber_context_t; // LPVOID
#else
# include <ucontext.h>
typedef ucontext_t fiber_context_t;
#endif
#include <functional>
#include <queue>

#include "xycommon.h"

class continuation;

class fiber final {
public:
    fiber(const fiber &) = delete;
    fiber &operator=(const fiber &) = delete;
    virtual ~fiber();
    static int yield(continuation &);
    static void launch(std::function<void()> entry,
                       size_t stack_size = 0x200000);

private:
    ucontext_t _context;
    char *_stack;
    size_t _stack_size;
    static ucontext_t _main_context;

    std::function<void()> _entry;
    bool _terminated;
    fiber *_prev;
    static fiber *_current;

    fiber(std::function<void()> entry, size_t stack_size);
    static void _wrapper(fiber *self);
    friend class continuation;
    void resume();
};

class continuation {
public:
    continuation();
    continuation(const continuation &) = delete;
    continuation &operator=(const continuation &) = delete;
    void resume(int status);
    inline operator bool() const { return _pending != nullptr; }
private:
    fiber *_pending;
    int _resume_status;
    friend class fiber;
};

#endif
