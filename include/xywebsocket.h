#ifndef XYHTTPD_XYWEBSOCKET_H
#define XYHTTPD_XYWEBSOCKET_H

#include "xystream.h"
#include <queue>

class websocket_frame : public message {
public:
    class decoder : public ::decoder {
    public:
        decoder(int maxPayloadLen);
        virtual bool decode(shared_ptr<streambuffer> &stb);
        virtual shared_ptr<message> msg();
    private:
        int _max_payload;
        shared_ptr<websocket_frame> _msg;
    };

    websocket_frame(int op, shared_ptr<string> payload);
    websocket_frame(int op, const char *payload, int len);
    virtual ~websocket_frame();
    inline int opcode() { return _op; }
    inline shared_ptr<string> payload() { return _payload; }

    virtual int type() const;
    virtual int serialize_size();
    virtual void serialize(char *buf);

private:
    int _op;
    shared_ptr<string> _payload;
};

class websocket {
public:
    websocket(shared_ptr<stream> strm);
    shared_ptr<string> read();
    void send(const string &str);
    void send(shared_ptr<string> str);
    virtual ~websocket();
    void flush_writing();
    void enq_message(shared_ptr<websocket_frame> msg);
private:
    shared_ptr<stream> _strm;
    shared_ptr<fiber> _flush_thread;
    bool _flush_paused, _ready;
    shared_ptr<websocket_frame::decoder> _decoder;
    queue<shared_ptr<websocket_frame>> _writing_queue;
};

#endif //XYHTTPD_XYWEBSOCKET_H
