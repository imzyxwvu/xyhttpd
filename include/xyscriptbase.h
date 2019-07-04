#ifndef XYHTTPD_XYSCRIPTBASE_H
#define XYHTTPD_XYSCRIPTBASE_H

#include "xycommon.h"


template <class T>
class script_object {
public:
    virtual void invoke_method() = 0;

    inline shared_ptr<T> get_instance() {
        return _inst;
    };
protected:
    shared_ptr<T> _inst;
    shared_ptr<class script_vm> _vm;
};

/*
 * The script_vm object represents a value-stack-based
 * scripting VM, such as lua_State or duk_context_t.
 */
class script_vm {
public:
    virtual void push_value(string &str) = 0;
    virtual void push_value(int n) = 0;
    virtual void push_value(bool b) = 0;

    virtual shared_ptr<string> to_string(int n) = 0;
    virtual int to_integer(int n) = 0;
    virtual bool to_boolean(int n) = 0;

    virtual ~script_vm();
};

class script_class {
public:
    typedef int (*meth)(shared<ptr> script_vm);
private:
    map<string, script_class::meth> _funcs;
};

#endif //XYHTTPD_XYSCRIPTBASE_H
