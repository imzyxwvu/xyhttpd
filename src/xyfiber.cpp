#include "xyfiber.h"

ucontext_t fiber::maincontext;
stack<shared_ptr<fiber>> fiber::levels;
shared_ptr<fiber> fiber::breathe;

wakeup_event::~wakeup_event() {}

fiber::fiber(void (*func)(void *)) : entry(func) {
    getcontext(&context);
    context.uc_stack.ss_sp = stack;
    context.uc_stack.ss_size = sizeof(stack);
    context.uc_link = NULL;
}

void fiber_wrapper(shared_ptr<fiber> *f, void *data)
{
    (*f)->invoke(data);
    delete f;
    fiber::yield();
}

shared_ptr<fiber> fiber::make(void (*entry)(void *), void *data) {
    shared_ptr<fiber> f(new fiber(entry));
    f->self = f;
    makecontext(&f->context, (void(*)(void))fiber_wrapper, 2, new shared_ptr<fiber>(f), data);
    f->terminated = false;
    return f;
}

void fiber::invoke(void *data) {
    try {
        entry(data);
    }
    catch(...) {}
    terminated = true;
    self.reset();
}

shared_ptr<wakeup_event> fiber::yield() {
    if(levels.empty())
        throw runtime_error("yielding outside a fiber");
    breathe = levels.top();
    levels.pop();
    shared_ptr<wakeup_event> &evt = breathe->event;
    swapcontext(&breathe->context,
                levels.empty() ? &maincontext : &levels.top()->context);
    return evt;
}

void fiber::resume() {
    if (!terminated) {
        ucontext_t *from = levels.empty() ?
                           &maincontext : &levels.top()->context;
        levels.push(self);
        swapcontext(from, &context);
    }
}