#ifndef XYHTTPD_XYWEBSOCKET_H
#define XYHTTPD_XYWEBSOCKET_H

#include "xystream.h"
#include "xymsgsink.h"
#include <queue>

class websocket_frame : public message {
public:
    class decoder : public ::decoder {
    public:
        decoder(int maxPayloadLen);
        virtual bool decode(stream_buffer &stb);
    private:
        int _max_payload;
    };

    websocket_frame(int op, shared_ptr<string> payload);
    websocket_frame(int op, const char *payload, int len);
    virtual ~websocket_frame();
    inline int opcode() { return _op & 0xf; }
    inline bool fin() { return (_op & 0x80) > 0; }
    inline bool deflated() { return (_op & 0x40) > 0; }
    inline shared_ptr<string> payload() { return _payload; }

    virtual int serialize_size();
    virtual void serialize(char *buf);

private:
    int _op;
    shared_ptr<string> _payload;
};

class websocket : public message_sink {
public:
    websocket(const shared_ptr<stream> &strm, bool _deflate);
    shared_ptr<string> read();
    virtual ~websocket();
    void flush_writing();
    void enq_message(const shared_ptr<websocket_frame> &msg);
    virtual void enq_message(const shared_ptr<string> &msg);

private:
    void cleanup();
    shared_ptr<stream> _strm;
    shared_ptr<fiber> _flush_thread;
    shared_ptr<string> _reassembled;
    bool _msg_deflated;
    bool _flush_paused;
    struct z_stream_s *_tx_zs, *_rx_zs;
    shared_ptr<websocket_frame::decoder> _decoder;
    queue<shared_ptr<websocket_frame>> _writing_queue;
};

#endif //XYHTTPD_XYWEBSOCKET_H
