#include <cstring>
#include <zlib.h>
#include "xywebsocket.h"

websocket_frame::decoder::decoder(int maxPayloadLen)
    : _max_payload(maxPayloadLen) {}

bool websocket_frame::decoder::decode(stream_buffer &stb) {
    unsigned char *frame = (unsigned char *)stb.data();
    int opcode_and_fin, payload_length, masked;
    int expectedLength = 2;
    if(stb.size() < expectedLength) return false;
    opcode_and_fin = frame[0];
    payload_length = frame[1] & 0x7f;
    if(masked = frame[1] & 0x80) expectedLength += 4;
    frame += 2;
    if(payload_length == 127) {
        expectedLength += 8;
        if(stb.size() < expectedLength) return false;
        if(frame[0] != 0 || frame[1] != 0 || frame[2] != 0 ||
           frame[3] != 0 || frame[4] != 0)
            throw runtime_error("payload too long");
        payload_length = (frame[5] << 16) + (frame[6] << 8) + frame[7];
        frame += 8;
    }
    else if(payload_length == 126) {
        expectedLength += 2;
        if(stb.size() < expectedLength) return false;
        payload_length = (frame[0] << 8) + frame[1];
        frame += 2;
    }
    if(payload_length > _max_payload) // Avoid DoS Attack of Giant Frame
        throw runtime_error("max payload length limit exceeded");
    expectedLength += payload_length;
    if(stb.size() < expectedLength) return false;
    if(payload_length > 0) {
        if(masked) {
            unsigned char *mask = frame;
            frame += 4;
            for(int i = 0; i < payload_length; i++)
                frame[i] ^= mask[i % 4];
        }
        _msg = make_shared<websocket_frame>(opcode_and_fin,
                                            (char *)frame, payload_length);
    } else {
        _msg = make_shared<websocket_frame>(opcode_and_fin, nullptr);
    }
    stb.pull(expectedLength);
    return true;
}

websocket_frame::websocket_frame(int op, shared_ptr<string> pl)
: _op(op), _payload(pl) {}

websocket_frame::websocket_frame(int op, const char *pl, int len)
: _op(op), _payload(make_shared<string>(pl, len)) {}

int websocket_frame::serialize_size() {
    int estimatedLength = 2;
    if(_payload) {
        if(_payload->size() > 0xffff)
            estimatedLength += 8;
        else if(_payload->size() > 125)
            estimatedLength += 2;
        estimatedLength += _payload->size();
    }
    return estimatedLength;
}

void websocket_frame::serialize(char *buf) {
    char *payloadBase = buf + 2;
    buf[0] = 0x80 | (_op & 0x4f);
    if(!_payload) {
        buf[1] = 0;
        return;
    }
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

websocket::websocket(const shared_ptr<stream> &strm, bool deflate) :
    _strm(strm), _decoder(make_shared<websocket_frame::decoder>(0x100000)),
    _tx_zs(nullptr), _rx_zs(nullptr), _msg_deflated(false) {
    if(deflate) {
        _rx_zs = new z_stream;
        _rx_zs->zalloc = nullptr;
        _rx_zs->zfree = nullptr;
        _rx_zs->opaque = nullptr;
        if(inflateInit2(_rx_zs, -MAX_WBITS) != Z_OK) {
            delete _rx_zs;
            throw runtime_error("failed to initialize z_stream");
        }
        _tx_zs = new z_stream;
        _tx_zs->zalloc = nullptr;
        _tx_zs->zfree = nullptr;
        _tx_zs->opaque = nullptr;
        if(deflateInit2(_tx_zs, Z_BEST_COMPRESSION, Z_DEFLATED, -MAX_WBITS,
                        MAX_MEM_LEVEL, Z_DEFAULT_STRATEGY) != Z_OK) {
            delete _tx_zs;
            _tx_zs = nullptr;
        }
    }
    _flush_thread = fiber::launch([this] () { this->flush_writing(); });
}

void websocket::flush_writing() {
    while(_alive) {
        _flush_paused = false;
        while(!_writing_queue.empty()) {
            auto msg = _writing_queue.front();
            _writing_queue.pop();
            try {
                _strm->write(msg);
            }
            catch(exception &ex) {
                _alive = false;
                return;
            }
        }
        _flush_paused = true;
        fiber::yield();
    }
    _flush_paused = false;
}

shared_ptr<string> websocket::read() {
    while(_alive) {
        try {
            auto frame = _strm->read<websocket_frame>(_decoder);
            if(!frame) {
                cleanup();
                return nullptr;
            }
            switch(frame->opcode()) {
                case 0:
                    if(!_reassembled)
                        throw runtime_error("unexpected WebSocket continuation");
                    if(frame->payload())
                        *_reassembled += *frame->payload();
                    break;
                case 1: case 2: // DATA OR TEXT
                    _reassembled = frame->payload();
                    _msg_deflated = frame->deflated();
                    break;
                case 8: // CLOSE
                    cleanup();
                    return nullptr;
                case 9: // PING
                    enq_message(make_shared<websocket_frame>(10, frame->payload()));
                    break;
            }
            if(frame->fin() && _reassembled) {
                if(_msg_deflated) {
                    if(!_rx_zs)
                        throw runtime_error("message is deflated");
                    *_reassembled += string("\0\0\xff\xff", 4);
                    stream_buffer sb;
                    _rx_zs->next_in = (Bytef *)_reassembled->data();
                    _rx_zs->avail_in = _reassembled->size();
                    while(_rx_zs->avail_in) {
                        _rx_zs->next_out = (Bytef *)sb.prepare(XY_PAGESIZE);
                        _rx_zs->avail_out = XY_PAGESIZE;
                        int r = 0;
                        if((r = inflate(_rx_zs, Z_SYNC_FLUSH)) != Z_OK)
                            throw runtime_error("inflate failure");
                        sb.commit(XY_PAGESIZE - _rx_zs->avail_out);
                    }
                    _reassembled = sb.to_string();
                    _msg_deflated = false;
                }
                return move(_reassembled);
            }
        }
        catch(runtime_error &ex) {
            cleanup();
            throw;
        }
    }
}

void websocket::enq_message(const shared_ptr<string> &str) {
    if(_tx_zs && !str->empty()) {
        _tx_zs->next_in = (Bytef *)str->data();
        _tx_zs->avail_in = str->size();
        stream_buffer sb;
        while(_tx_zs->avail_in) {
            _tx_zs->next_out = (Bytef *)sb.prepare(XY_PAGESIZE);
            _tx_zs->avail_out = XY_PAGESIZE;
            if(deflate(_tx_zs, Z_SYNC_FLUSH) != Z_OK)
                throw runtime_error("deflate failure");
            sb.commit(XY_PAGESIZE - _tx_zs->avail_out);
        }
        enq_message(make_shared<websocket_frame>(0x41, sb.data(), sb.size() - 4));
    } else {
        enq_message(make_shared<websocket_frame>(1, str));
    }
}

void websocket::enq_message(const shared_ptr<websocket_frame> &msg) {
    _writing_queue.push(msg);
    if(_flush_paused) _flush_thread->resume();
}

void websocket::cleanup() {
    _alive = false;
    if(_flush_paused) _flush_thread->resume();
    _reassembled.reset();
    _flush_thread.reset();
    _strm.reset();
    if(_rx_zs) {
        inflateEnd(_rx_zs);
        delete _rx_zs;
        _rx_zs = nullptr;
    }
    if(_tx_zs) {
        deflateEnd(_tx_zs);
        delete _tx_zs;
        _tx_zs = nullptr;
    }

}

websocket::~websocket() { cleanup(); }