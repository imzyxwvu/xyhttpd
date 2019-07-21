#include <iostream>
#include <sys/mman.h>
#include <unistd.h>
#include "xyfiber.h"

ucontext_t fiber::maincontext;
stack<shared_ptr<fiber>> fiber::levels;
shared_ptr<fiber> fiber::_last_breathe;

wakeup_event::~wakeup_event() {}

static void fiber_wrapper(fiber *f, void *data)
{
    f->invoke(data);
    fiber::finalize();
}

fiber::fiber(size_t stacksiz, void (*func)(void *), void *data)
: _stack_size(stacksiz), entry(func) {
    _stack = (char *)mmap(NULL, stacksiz, PROT_READ | PROT_WRITE,
            MAP_PRIVATE | MAP_ANON, -1, 0);
    if(!_stack)
        throw RTERR("cannot allocate stack space for fiber");
    *((ucontext_t **)_stack) = &context;
    mprotect(_stack, getpagesize(), PROT_READ);
    getcontext(&context);
    context.uc_stack.ss_sp = _stack;
    context.uc_stack.ss_size = _stack_size;
    context.uc_link = NULL;
    makecontext(&context, (void(*)(void))fiber_wrapper, 2, this, data);
    _terminated = false;
}

fiber::~fiber() {
    munmap(_stack, _stack_size);
}

void fiber::finalize() {
    _last_breathe = levels.top();
    levels.pop();
    swapcontext(&_last_breathe->context,
                levels.empty() ? &maincontext : &levels.top()->context);
}

shared_ptr<fiber> fiber::make(void (*entry)(void *), void *data) {
    shared_ptr<fiber> f(new fiber(0x100000, entry, data));
    f->self = f;
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
