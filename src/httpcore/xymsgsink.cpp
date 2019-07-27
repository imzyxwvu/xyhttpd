#include "xymsgsink.h"

message_sink::message_sink() : _alive(true) {}

void message_sink::send(shared_ptr<string> str) {
    if(!_alive)
        throw RTERR("writing message to inactive message sink");
    enq_message(str);
}

message_sink::~message_sink() {}

void message_broadcaster::enq_message(shared_ptr<string> msg) {
    broadcast(msg, nullptr);
}

void message_broadcaster::broadcast(shared_ptr<string> msg, shared_ptr<message_sink> except) {
    _nsucc = 0;
    for(auto it = _sinks.begin(); it != _sinks.end();) {
        if((*it)->alive()) {
            if(*it == except)
                continue;
            try {
                (*it)->send(msg);
                _nsucc++;
            }
            catch(runtime_error &err) {}
            it++;
        } else {
            it = _sinks.erase(it);
        }
    }
}

void message_broadcaster::add_sink(shared_ptr<message_sink> sink) {
    for(auto it = _sinks.begin(); it != _sinks.end();) {
        if ((*it)->alive())
            it++;
        else
            it = _sinks.erase(it);
    }
    if(sink->alive())
        _sinks.push_back(sink);
}

void message_broadcaster::clear() {
    _sinks.clear();
}