#ifndef XYHTTPD_FIBER_H
#define XYHTTPD_FIBER_H

#include <ucontext.h>
#include <stack>
#include <queue>

#include "xycommon.h"

class wakeup_event {
public:
    virtual ~wakeup_event() = 0;
};

class fiber {
public:
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

    shared_ptr<wakeup_event> event;
    fiber(void (*func)(void *), void *data);
    template<class T>
    inline static shared_ptr<T> yield() {
        return dynamic_pointer_cast<T>(yield());
    }
    static shared_ptr<wakeup_event> yield();
    void resume();
    void raise(const string &ex);
    inline void resume(shared_ptr<wakeup_event> evt) {
        event = move(evt);
        resume();
    }
    static shared_ptr<fiber> make(void (*func)(void *), void *data);
    void invoke(void *data);
    inline static shared_ptr<fiber> current() {
        return levels.top();
    }
    inline static bool in_fiber() {
        return !levels.empty();
    }
    ~fiber();
private:
    fiber(const fiber &) = delete;
    static void wrapper(fiber *f, void *data);
    ucontext_t context;
    bool _terminated;
    void (*entry)(void *data);
    shared_ptr<fiber> self;
    shared_ptr<stack_mem> _stack;
    shared_ptr<runtime_error> _err;
    static shared_ptr<fiber> _last_breathe;
    static std::stack<shared_ptr<fiber>> levels;
    static ucontext_t maincontext;

    static queue<shared_ptr<stack_mem>> stack_pool;
    static int stack_pool_target;
};

#endif
