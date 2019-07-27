#ifndef XYHTTPD_XYMSGSINK_H
#define XYHTTPD_XYMSGSINK_H

#include <vector>
#include <queue>
#include "xycommon.h"

class message_sink {
public:
    message_sink();
    message_sink(const message_sink &) = delete;
    inline void send(const string &str) {
        send(make_shared<string>(str));
    }
    void send(shared_ptr<string> str);
    inline bool alive() { return _alive; }
    virtual ~message_sink() = 0;
protected:
    virtual void enq_message(shared_ptr<string> msg) = 0;
    bool _alive;
};

class message_broadcaster : public message_sink {
public:
    message_broadcaster() = default;
    virtual void enq_message(shared_ptr<string> msg);
    void broadcast(shared_ptr<string> msg, shared_ptr<message_sink> except);
    void add_sink(shared_ptr<message_sink> sink);
    int size() { return _sinks.size(); }
    int success_count() { return _nsucc; }
    void clear();
private:
    vector<shared_ptr<message_sink>> _sinks;
    int _nsucc;
};

#endif //XYHTTPD_XYMSGSINK_H
