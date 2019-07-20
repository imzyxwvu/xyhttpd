#include <iostream>
#include "xyfiber.h"

ucontext_t fiber::maincontext;
stack<shared_ptr<fiber>> fiber::levels;
shared_ptr<fiber> fiber::_last_breathe;

wakeup_event::~wakeup_event() {}

fiber::fiber(void (*func)(void *)) : entry(func) {
    getcontext(&context);
    context.uc_stack.ss_sp = stack;
    context.uc_stack.ss_size = sizeof(stack);
    context.uc_link = NULL;
}

static void fiber_wrapper(fiber *f, void *data)
{
    f->invoke(data);
    fiber::finalize();
}

void fiber::finalize() {
    _last_breathe = levels.top();
    levels.pop();
    swapcontext(&_last_breathe->context,
                levels.empty() ? &maincontext : &levels.top()->context);
}

shared_ptr<fiber> fiber::make(void (*entry)(void *), void *data) {
    shared_ptr<fiber> f(new fiber(entry));
    f->self = f;
    makecontext(&f->context, (void(*)(void))fiber_wrapper, 2, f.get(), data);
    f->_terminated = false;
    return f;
}

void fiber::invoke(void *data) {
    try {
        entry(data);
    }
    catch(exception &ex) {
        cerr<<"["<<timelabel()<<" "<<this<<"] "<<ex.what()<<endl;
    }
    _terminated = true;
    self.reset();
}

shared_ptr<wakeup_event> fiber::yield() {
    if(levels.empty())
        throw RTERR("yielding outside a fiber");
    shared_ptr<fiber> self = levels.top();
    levels.pop();
    swapcontext(&self->context,
                levels.empty() ? &maincontext : &levels.top()->context);
    if(self->_err) {
        runtime_error ex(*self->_err);
        self->_err.reset();
        throw ex;
    }
    return self->event;
}

void fiber::resume() {
    if(_terminated)
        throw RTERR("resuming terminated fiber");
    ucontext_t *from = levels.empty() ?
                       &maincontext : &levels.top()->context;
    levels.push(self);
    swapcontext(from, &context);
    _last_breathe.reset();
}

void fiber::raise(const string &ex) {
    _err = make_shared<extended_runtime_error>("@fiber", 0, ex);
    resume();
}