#ifndef XYHTTPD_XYWEBSOCKET_H
#define XYHTTPD_XYWEBSOCKET_H

#include "xystream.h"
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

    websocket_frame(int op, chunk payload);
    virtual ~websocket_frame();
    inline int opcode() { return _op & 0xf; }
    inline bool fin() { return (_op & 0x80) > 0; }
    inline bool deflated() { return (_op & 0x40) > 0; }
    inline chunk payload() { return _payload; }

    virtual int serialize_size();
    virtual void serialize(char *buf);

private:
    int _op;
    chunk _payload;
};

class websocket {
public:
    websocket(const P<stream> &strm, bool _deflate);
    bool poll();
    chunk read();
    virtual ~websocket();
    virtual void send(const chunk &msg);

private:
    void cleanup();
    void flush_writing();
    P<stream> _strm;
    P<fiber> _flush_thread;
    stream_buffer _reassembled;
    chunk _done;
    bool _msg_deflated;
    bool _flush_paused, _alive;
    struct z_stream_s *_tx_zs, *_rx_zs;
    P<websocket_frame::decoder> _decoder;
    std::queue<P<websocket_frame>> _writing_queue;
};

#endif //XYHTTPD_XYWEBSOCKET_H
