#ifndef XYHTTPD_FIBER_H
#define XYHTTPD_FIBER_H

#include <ucontext.h>
#include <stack>

#include "xycommon.h"

class wakeup_event {
public:
    virtual ~wakeup_event() = 0;
};

class fiber {
public:
    shared_ptr<wakeup_event> event;
    template<class T>
    inline static shared_ptr<T> yield() {
        return dynamic_pointer_cast<T>(yield());
    }
    static shared_ptr<wakeup_event> yield();
    static void finalize();
    void resume();
    void raise(const string &ex);
    inline void resume(shared_ptr<wakeup_event> evt) {
        event = evt;
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
private:
    fiber(void (*func)(void *));
    fiber(const fiber &);
    ucontext_t context;
    char stack[0x20000];
    bool _terminated;
    void (*entry)(void *data);
    shared_ptr<fiber> self;
    shared_ptr<runtime_error> _err;
    static shared_ptr<fiber> _last_breathe;
    static std::stack<shared_ptr<fiber>> levels;
    static ucontext_t maincontext;
};

#endif
