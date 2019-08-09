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

class wakeup_event {
public:
    virtual ~wakeup_event() = 0;
};

class fiber {
private:
#ifndef _WIN32
    class stack_mem {
    public:
        explicit stack_mem(int _size);
        stack_mem(const stack_mem &) = delete;
        ~stack_mem();
        inline void *base() { return _base; }
        inline size_t size() { return _size; }
    private:
        char *_base;
        size_t _size;
    };

    shared_ptr<stack_mem> _stack;
    static queue<shared_ptr<stack_mem>> stack_pool;
#endif

public:
    class preserve {
    public:
        explicit preserve(shared_ptr<fiber> &f);
        ~preserve();
    private:
        shared_ptr<fiber> &_f;
    };

    fiber(const fiber &) = delete;
    explicit fiber(function<void()>);
    template<class T>
    inline static shared_ptr<T> yield() {
        return dynamic_pointer_cast<T>(yield());
    }
    static shared_ptr<wakeup_event> yield();
    void resume();
    void raise(const string &ex);
    inline void resume(shared_ptr<wakeup_event> evt) {
        _event = move(evt);
        resume();
    }
    static shared_ptr<fiber> launch(function<void()>);
    inline static shared_ptr<fiber> current() {
        return _current;
    }
    ~fiber();
private:
    static void wrapper(fiber *f);
    fiber_context_t context;
    shared_ptr<wakeup_event> _event;
    bool _terminated;
    function<void()> _entry;
    shared_ptr<fiber> self, _prev;
    shared_ptr<runtime_error> _err;
    static shared_ptr<fiber> _current;
    static fiber_context_t maincontext;

    static int stack_pool_target;
};

#endif
