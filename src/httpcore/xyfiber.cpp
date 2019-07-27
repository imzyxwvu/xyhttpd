#include <iostream>
#include <sys/mman.h>
#include <unistd.h>
#include "xyfiber.h"

ucontext_t fiber::maincontext;
stack<shared_ptr<fiber>> fiber::levels;
shared_ptr<fiber> fiber::_last_breathe;

wakeup_event::~wakeup_event() {}

queue<shared_ptr<fiber::stack_mem>> fiber::stack_pool;
int fiber::stack_pool_target = 32;

fiber::stack_mem::stack_mem(int siz) : _size(siz) {
    _base = (char *)mmap(NULL, _size, PROT_READ | PROT_WRITE,
                          MAP_PRIVATE | MAP_ANON, -1, 0);
    if(!_base)
        throw RTERR("cannot allocate stack space for fiber");
    mprotect(_base, getpagesize(), PROT_NONE);
}

fiber::stack_mem::~stack_mem() {
    munmap(_base, _size);
}

fiber::fiber(void (*func)(void *), void *data)
: entry(func) {
    if(stack_pool.empty()) {
        _stack = make_shared<stack_mem>(0x200000);
    } else {
        _stack = stack_pool.front();
        stack_pool.pop();
    }
    getcontext(&context);
    context.uc_stack.ss_sp = _stack->base();
    context.uc_stack.ss_size = _stack->size();
    context.uc_link = NULL;
    makecontext(&context, (void(*)(void))fiber::wrapper, 2, this, data);
    _terminated = false;
}

fiber::~fiber() {
    if(stack_pool.size() < stack_pool_target)
        stack_pool.push(_stack);
}

void fiber::wrapper(fiber *f, void *data) {
    f->invoke(data);
    _last_breathe = levels.top();
    levels.pop();
    swapcontext(&_last_breathe->context,
                levels.empty() ? &maincontext : &levels.top()->context);
}

shared_ptr<fiber> fiber::make(void (*entry)(void *), void *data) {
    auto f = make_shared<fiber>(entry, data);
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
