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

    P<stack_mem> _stack;
    static std::queue<P<stack_mem>> stack_pool;
#endif

public:
    class preserve {
    public:
        explicit preserve(P<fiber> &f);
        ~preserve();
    private:
        P<fiber> &_f;
    };

    fiber(const fiber &) = delete;
    explicit fiber(std::function<void()>);
    template<class T>
    inline static P<T> yield() {
        return std::dynamic_pointer_cast<T>(yield());
    }
    static P<wakeup_event> yield();
    void resume();
    void raise(const std::string &ex);
    inline void resume(P<wakeup_event> evt) {
        _event = move(evt);
        resume();
    }
    static P<fiber> launch(std::function<void()>);
    inline static P<fiber> current() {
        return _current;
    }
    ~fiber();
private:
    static void wrapper(fiber *f);
    fiber_context_t context;
    P<wakeup_event> _event;
    bool _terminated;
    std::function<void()> _entry;
    P<fiber> self, _prev;
    P<std::runtime_error> _err;
    static std::shared_ptr<fiber> _current;
    static fiber_context_t maincontext;

    static int stack_pool_target;
};

#endif
