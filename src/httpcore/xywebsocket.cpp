#include <cstring>
#include "xywebsocket.h"

websocket_frame::decoder::decoder(int maxPayloadLen)
    : _max_payload(maxPayloadLen) {}

bool websocket_frame::decoder::decode(shared_ptr<streambuffer> &stb) {
    char *frame = stb->data();
    int opcode_and_fin, payload_length, masked;
    char mask[4];
    int expectedLength = 2;
    int i;
    if(stb->size() < expectedLength) return false;
    opcode_and_fin = frame[0];
    payload_length = frame[1] & 0x7f;
    if(masked = frame[1] & 0x80) expectedLength += 4;
    frame += 2;
    if(payload_length == 127) {
        expectedLength += 8;
        if(stb->size() < expectedLength) return false;
        if(frame[0] != 0 || frame[1] != 0 || frame[2] != 0 ||
           frame[3] != 0 || frame[4] != 0)
            throw runtime_error("payload too long");
        payload_length = frame[5] * 0x10000 + frame[6] * 0x100 + frame[7];
        frame += 8;
    }
    else if(payload_length == 126) {
        expectedLength += 2;
        if(stb->size() < expectedLength) return false;
        payload_length = frame[0] * 0x100 + frame[1];
        frame += 2;
    }
    if(payload_length > _max_payload) // Avoid DoS Attack of Giant Frame
        throw runtime_error("max payload length limit exceeded");
    expectedLength += payload_length;
    if(stb->size() < expectedLength) return false;
    if(payload_length > 0) {
        if(masked) {
            *(uint32_t *)mask = *(uint32_t *)frame;
            frame += 4;
            for(i = 0; i < payload_length; i++) frame[i] ^= mask[i % 4];
        }
        _msg = make_shared<websocket_frame>(opcode_and_fin & 0xf,
                                            frame, payload_length);
    } else {
        _msg = make_shared<websocket_frame>(opcode_and_fin & 0xf, nullptr);
    }
    stb->pull(expectedLength);
    return 1;
}

shared_ptr<message> websocket_frame::decoder::msg() {
    return _msg;
}

websocket_frame::websocket_frame(int op, shared_ptr<string> pl)
: _op(op), _payload(pl) {}

websocket_frame::websocket_frame(int op, const char *pl, int len)
: _op(op), _payload(new string(pl, len)) {}

int websocket_frame::type() const {
    return XY_MESSAGE_WSFRAME;
}

int websocket_frame::serialize_size() {
    int estimatedLength = 2;
    if(_payload->size() > 0xffff)
        estimatedLength += 8;
    else if(_payload->size() > 125)
        estimatedLength += 2;
    estimatedLength += _payload->size();
    return estimatedLength;
}

void websocket_frame::serialize(char *buf) {
    char *payloadBase = buf + 2;
    buf[0] = 0x80 | (_op & 0xf);
    if(!_payload)
        buf[1] = 0;
    else if(_payload->size() > 0xffff) {
        buf[1] = 127;
        buf[2] = buf[3] = buf[4] = buf[5] = 0;
        buf[6] = (_payload->size() >> 24) & 0x7f;
        buf[7] = (_payload->size() >> 16) & 0xff;
        buf[8] = (_payload->size() >> 8) & 0xff;
        buf[9] = _payload->size() & 0xff;
        payloadBase += 8;
    }
    else if(_payload->size() > 125) {
        buf[1] = 126;
        buf[2] = (_payload->size() >> 8) & 0xff;
        buf[3] = _payload->size() & 0xff;
        payloadBase += 2;
    } else {
        buf[1] = _payload->size();
    }
    memcpy(payloadBase, _payload->data(), _payload->size());

}

websocket_frame::~websocket_frame() {}

static void websocket_flush_thread(void *data) {
    websocket *ws = (websocket *)data;
    ws->flush_writing();
}

websocket::websocket(shared_ptr<stream> strm) :
    _strm(strm), _ready(true),
    _decoder(new websocket_frame::decoder(0x100000)) {
    _flush_thread = fiber::make(websocket_flush_thread, this);
    _flush_thread->resume();
}

void websocket::flush_writing() {
    while(_ready) {
        _flush_paused = false;
        while(!_writing_queue.empty()) {
            auto msg = _writing_queue.front();
            _writing_queue.pop();
            _strm->write(msg);
        }
        _flush_paused = true;
        fiber::yield();
    }
}

shared_ptr<string> websocket::read() {
    while(_ready) {
        auto frame = _strm->read<websocket_frame>(_decoder);
        switch(frame->opcode()) {
            case 0: case 1: // DATA OR TEXT
                return frame->payload();
            case 8: // CLOSE
                _ready = false;
                if(_flush_paused) _flush_thread->resume();
                return nullptr;
            case 9: // PING
                enq_message(make_shared<websocket_frame>(10, frame->payload()));
                break;
            default:
                throw RTERR("unknown WebSocket opcode - %d", frame->opcode());
        }
    }
}

void websocket::send(const string &str) {
    enq_message(make_shared<websocket_frame>(1, make_shared<string>(str)));
}

void websocket::send(shared_ptr<string> str) {
    enq_message(make_shared<websocket_frame>(1, str));
}

void websocket::enq_message(shared_ptr<websocket_frame> msg) {
    _writing_queue.push(msg);
    if(_flush_paused) _flush_thread->resume();
}

websocket::~websocket() {}