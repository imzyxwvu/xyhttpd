#include <iostream>
#include <cstring>
#include <unistd.h>

#include "xyhttp.h"

#include <zlib.h>
#include <openssl/sha.h> // used by http_transaction::accept_websocket
#include <cassert>
#include <ctime>

using namespace std;

const string http_transaction::SERVER_VERSION("xyhttpd/19.07 (xwsg.0xspot.com)");
const string http_transaction::WEBSOCKET_MAGIC("258EAFA5-E914-47DA-95CA-C5AB0DC85B11");

http_transaction::http_transaction(
    shared_ptr<http_connection> conn, shared_ptr<http_request> req) :
    connection(move(conn)), request(move(req)), _headerSent(false),
    _finished(false), _transfer_mode(UNDECIDED), _gzip(nullptr) {
    _response = make_shared<http_response>(200);
    auto contentLength = request->header("content-length");
    if(contentLength) {
        int len = atoi(contentLength.data());
        if(len > 0x800000 || len < 0) {
            display_error(413);
            throw RTERR("request body too long (Content-Length)");
        }
        char *buf = new char[len];
        char *bufpos = buf;
        auto dec = make_shared<string_decoder>(len);
        while(dec->more()) {
            auto str = connection->_strm->read<string_message>(dec);
            memcpy(bufpos, str->data(), str->serialize_size());
            bufpos += str->serialize_size();
        }
        postdata = chunk(buf, len);
        delete[] buf;
    }
    if(request->method == "HEAD")
        _transfer_mode = HEADONLY;
    _noGzip = !request->header_include("accept-encoding", "gzip");
}

http_transaction::~http_transaction() {
    if(_gzip) {
        deflateEnd(_gzip);
        delete _gzip;
        _gzip = nullptr;
    }
}

void http_transaction::forward_to(const string &host, int port) {
    auto upstream = make_shared<tcp_stream>();
    upstream->connect(host, port);
    return forward_to(upstream);
}

void http_transaction::forward_to(P<stream> strm) {
    if(header_sent()) throw RTERR("header already sent");
    auto req = make_shared<http_request>(*request);
    req->set_header("X-Forwarded-For", connection->_peername);
    strm->write(req);
    if(postdata) strm->write(postdata);
    _response->set_code(100);
    auto respdec = make_shared<http_response::decoder>();
    while(_response->code() == 100) {
        auto upstream_response = strm->read<http_response>(respdec);
        if (!upstream_response) {
            display_error(502);
            return;
        }
        _response = move(upstream_response);
    }
    if(_response->header("Content-Encoding"))
        _noGzip = true; // Disable GZIP if upstream has already done compression
    if (_response->code() == 101 && _response->header("Upgrade")) {
        auto client = upgrade();
        client->pipe(strm);
        strm->pipe(client);
        return;
    }
    else if (_response->code() != 204 && _response->code() != 304) {
        auto dec = make_shared<http_transfer_decoder>(_response);
        _response->delete_header("Transfer-Encoding");
        try {
            while(dec->more()) {
                auto msg = strm->read<string_message>(dec);
                write(msg->data(), msg->serialize_size());
            }
        }
        catch(runtime_error &ex) { } // EOF won't be thrown outside
    }
    finish();
}

void http_transaction::forward_to(P<fcgi_connection> conn) {
    conn->set_env("PATH_INFO", request->path());
    conn->set_env("SERVER_PROTOCOL", "HTTP/1.1");
    conn->set_env("CONTENT_TYPE", request->header("content-type"));
    conn->set_env("CONTENT_LENGTH", request->header("content-length"));
    conn->set_env("SERVER_SOFTWARE", SERVER_VERSION);
    conn->set_env("REQUEST_URI", request->resource());
    if(request->query())
        conn->set_env("QUERY_STRING", request->query());
    conn->set_env("REQUEST_METHOD", request->method);
    conn->set_env("REMOTE_ADDR", connection->peername());
    if(connection->_strm->has_tls())
        conn->set_env("HTTPS", "on");
    for(auto it = request->hbegin(); it != request->hend(); it++) {
        char envKeyBuf[64] = "HTTP_";
        char *dest = envKeyBuf + 5;
        for(const char *src = it->first.c_str(); *src; src++)
            *(dest++) = (*src == '-') ? '_' : toupper(*src);
        conn->set_env(envKeyBuf, it->second);
    }
    if(postdata) conn->write(postdata);
    stream_buffer responseBuffer;
    while(true) {
        chunk data = conn->read();
        if(!data) {
            display_error(502);
            return;
        }
        responseBuffer.append(data.data(), data.size());
        auto respDecoder = make_shared<http_response::decoder>();
        if(respDecoder->decode(responseBuffer)) {
            _response = dynamic_pointer_cast<http_response>(respDecoder->msg());
            break;
        }
    }
    if(responseBuffer.size() > 0) {
        write(responseBuffer.data(), responseBuffer.size());
        responseBuffer.pull(responseBuffer.size());
    }
    while(true) {
        auto msg = conn->read();
        if(!msg) break;
        write(msg.data(), msg.size());
    }
    finish();
}

void http_transaction::serve_file(const string &filename) {
    if(request->method != "GET" && request->method != "HEAD") {
        display_error(405);
        return;
    }
    struct stat info;
    if(stat(filename.c_str(), &info)) {
        switch(errno) {
        case EACCES: display_error(403); return;
        default: display_error(404); return;
        }
    }
    if(!S_ISREG(info.st_mode)) {
        display_error(405);
        return;
    }
    char ftbuf[64];
    int tlen = ::strftime(ftbuf, sizeof(ftbuf),
        "%a, %d %b %Y %H:%M:%S GMT", ::gmtime(&info.st_mtime));
    string modtime(ftbuf, tlen);
    chunk chktime = request->header("if-modified-since");
    if(chktime && modtime == chktime) {
        auto resp = get_response(304);
        finish();
        return;
    }
    _response->set_header("Last-Modified", modtime);
    if(_transfer_mode == HEADONLY) {
        _response->set_header("Content-Length", to_string(info.st_size));
        finish();
        return;
    }
    serve_file(filename, info);
}

void http_transaction::serve_file(const string &filename, struct stat &info) {
    if(header_sent()) throw runtime_error("header already sent");
    auto range = request->header("range");
    size_t seekTo = 0, rest = info.st_size;
    if(range && range.size() > 6 && memcmp(range.data(), "bytes=", 6) == 0) {
        string byteRange(range.data() + 6, range.size() - 6);
        int sep = byteRange.find('-');
        if(sep > 0) {
            seekTo = stol(byteRange.substr(0, sep));
            int endPos = info.st_size - 1;
            if(sep < byteRange.size() - 1)
                endPos = stol(byteRange.substr(sep + 1));
            if(!(endPos < info.st_size && seekTo <= endPos)) {
                display_error(416);
                return;
            }
            rest = endPos - seekTo + 1;
        }
    }
    int fd = open(filename.c_str(), O_RDONLY);
    if(fd < 0) {
        display_error(403);
        return;
    }
    if(rest < info.st_size) {
        _response->set_code(206);
        _response->set_header("Content-Range",
                         fmt("bytes %d-%d/%d", seekTo, seekTo + rest - 1, info.st_size));
        if(lseek(fd, seekTo, SEEK_SET) == -1) {
            close(fd);
            display_error(416);
            return;
        }
    }
    if(_noGzip) {
        _response->set_header("Content-Length", to_string(rest));
        _tx_buffer.pull(_tx_buffer.size());
        start_transfer(SIMPLE);
    }
#ifdef _WIN32
    size_t chunkSize = 8192;
#else
    size_t chunkSize = max<size_t>(info.st_blksize, 8192) * 2;
#endif
    char *buf = new char[chunkSize];
    while(rest > 0) {
        size_t avail = read(fd, buf, min<size_t>(chunkSize, rest));
        if(avail > 0) {
            this->write(buf, avail);
            rest -= avail;
        } else {
            connection->_keep_alive = false;
            break;
        }
    }
    delete[] buf;
    close(fd);
    finish();
}

shared_ptr<stream> http_transaction::upgrade(bool flush_resp) {
    if(_transfer_mode != UNDECIDED || _gzip)
        throw runtime_error("transaction not in clean state");
    _tx_buffer.pull(_tx_buffer.size());
    if(flush_resp) {
        start_transfer(UPGRADE);
    } else {
        _transfer_mode = UPGRADE;
        _headerSent = true;
    }
    _finished = true;
    return connection->upgrade();
}

P<websocket> http_transaction::accept_websocket() {
    if(request->method != "GET") {
        display_error(405);
        throw RTERR("WebSocket negotiation expects GET, got %s",
                    request->method.c_str());
    }
    auto wskey = request->header("sec-websocket-key");
    if(!request->header("upgrade") || !wskey)
        throw RTERR("Headers necessary for WebSocket handshake not present");
    string wsaccept = wskey.substr(0) + WEBSOCKET_MAGIC;
    unsigned char shabuf[SHA_DIGEST_LENGTH];
    SHA1((unsigned char *)wsaccept.data(), wsaccept.size(), shabuf);
    auto resp = get_response(101);
    bool permsgDeflate;
    resp->set_header("Upgrade", "websocket");
    resp->set_header("Sec-WebSocket-Accept", base64_encode(shabuf, SHA_DIGEST_LENGTH));
    permsgDeflate = request->header_include("sec-websocket-extensions", "permessage-deflate")
            && !request->header_include("sec-websocket-extensions", "server_max_window_bits");
    if(permsgDeflate)
        resp->set_header("Sec-WebSocket-Extensions", "permessage-deflate");
    return make_shared<websocket>(upgrade(), permsgDeflate);
}

void http_transaction::redirect_to(const string &dest) {
    auto resp = get_response(302);
    resp->set_header("Location", dest);
    finish();
}

void http_transaction::display_error(int code) {
    auto resp = get_response(code);
    if(_transfer_mode == UNDECIDED && _tx_buffer.size() == 0) {
        resp->set_header("Content-Type", "text/html");
        write(fmt("<html><head><title>XWSG Error %d</title></head>"
                  "<body><h1>%d %s</h1></body></html>", code, code,
                  http_response::state_description(code)));
    }
    finish();
}

shared_ptr<http_response> http_transaction::get_response() {
    return _response;
}

shared_ptr<http_response> http_transaction::get_response(int code) {
    if(header_sent())
        throw runtime_error("header already sent");
    _response->set_code(code);
    return _response;
}

void http_transaction::start_transfer(transfer_mode mode) {
    assert(!_headerSent);
    _transfer_mode = mode;
    _response->set_header("Server", SERVER_VERSION);
    if(_transfer_mode == UPGRADE) {
        _response->set_header("Connection", "upgrade");
    } else {
        _response->set_header("Connection",
                              connection->keep_alive() ? "keep-alive" : "close");
        if(_transfer_mode == CHUNKED) {
            _response->delete_header("Content-Length");
            _response->set_header("Transfer-Encoding", "chunked");
        }
    }
    connection->_strm->write(_response);
    _headerSent = true;
}

void http_transaction::write(const char *buf, int len) {
    if(_finished)
        throw RTERR("writing to finished transaction");
    if(len == 0)
        return;
    else if(_gzip) {
        Bytef outBuf[0x4000];
        _gzip->next_in = (Bytef *)buf;
        _gzip->avail_in = len;
        while(_gzip->avail_in > 0) {
            _gzip->next_out = outBuf;
            _gzip->avail_out = sizeof(outBuf);
            int ret = deflate(_gzip, 0);
            if(ret != Z_OK)
                throw runtime_error("GZIP compression failure");
            if(sizeof(outBuf) - _gzip->avail_out > 0)
                transfer((char *)&outBuf, sizeof(outBuf) - _gzip->avail_out);
        }
    } else {
        transfer(buf, len);
        if(_transfer_mode == UNDECIDED && !_noGzip && _tx_buffer.size() >= 0x200) {
            _gzip = new z_stream;
            _gzip->zalloc = Z_NULL;
            _gzip->zfree = Z_NULL;
            _gzip->opaque = Z_NULL;
            if(deflateInit2(_gzip, Z_BEST_COMPRESSION, Z_DEFLATED,
                            MAX_WBITS + 16, MAX_MEM_LEVEL, Z_DEFAULT_STRATEGY) != Z_OK) {
                delete _gzip;
                _gzip = nullptr;
                return;
            }
            _response->set_header("Content-Encoding", "gzip");
            len = _tx_buffer.size();
            buf = _tx_buffer.detach();
            write(buf, len);
            free((void *)buf);
        }
    }
}

void http_transaction::transfer(const char *buf, int len) {
    switch(_transfer_mode) {
        case SIMPLE:
            connection->_strm->write(buf, len);
            break;
        case UNDECIDED:
            _tx_buffer.append(buf, len);
            if(_tx_buffer.size() > 0x20000) {
                start_transfer(CHUNKED);
                transfer(_tx_buffer.data(), _tx_buffer.size());
                _tx_buffer.pull(_tx_buffer.size());
            }
            break;
        case CHUNKED: {
            char chunkHdr[16];
            int hdrLen = sprintf(chunkHdr, "%x\r\n", len);
            connection->_strm->write(chunkHdr, hdrLen);
            connection->_strm->write(buf, len);
            connection->_strm->write("\r\n", 2);
            break;
        }
        default: break;
    }
}

void http_transaction::write(const string &buf) {
    write(buf.data(), buf.size());
}

void http_transaction::finish() {
    if(_finished) return;
    if(_gzip) { // Flush data buffered in z_stream if any
        Bytef outBuf[16384];
        while(true) {
            _gzip->next_out = outBuf;
            _gzip->avail_out = sizeof(outBuf);
            int ret = deflate(_gzip, Z_FINISH);
            if(ret != Z_OK && ret != Z_STREAM_END)
                throw runtime_error("GZIP compression failure");
            transfer((char *)&outBuf, sizeof(outBuf) - _gzip->avail_out);
            if(ret == Z_STREAM_END) break;
        }
    }
    if(_transfer_mode == CHUNKED)
        connection->_strm->write("0\r\n\r\n", 5);
    else {
        if(_response->code() == 204 || _response->code() == 304)
            start_transfer(HEADONLY);
        else if(_transfer_mode == UNDECIDED) {
            _response->set_header("Content-Length", to_string(_tx_buffer.size()));
            start_transfer(SIMPLE);
            if(_tx_buffer.size() > 0) {
                transfer(_tx_buffer.data(), _tx_buffer.size());
                _tx_buffer.pull(_tx_buffer.size());
            }
        }
    }
    _finished = true;
}

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
        _msg = make_shared<websocket_frame>(opcode_and_fin, chunk((char *)frame, payload_length));
    } else {
        _msg = make_shared<websocket_frame>(opcode_and_fin, nullptr);
    }
    stb.pull(expectedLength);
    return true;
}

websocket_frame::websocket_frame(int op, chunk pl) : _op(op), _payload(pl) {}

int websocket_frame::serialize_size() {
    int estimatedLength = 2;
    if(_payload) {
        if(_payload.size() > 0xffff)
            estimatedLength += 8;
        else if(_payload.size() > 125)
            estimatedLength += 2;
        estimatedLength += _payload.size();
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
    else if(_payload.size() > 0xffff) {
        buf[1] = 127;
        buf[2] = buf[3] = buf[4] = buf[5] = 0;
        buf[6] = (_payload.size() >> 24) & 0x7f;
        buf[7] = (_payload.size() >> 16) & 0xff;
        buf[8] = (_payload.size() >> 8) & 0xff;
        buf[9] = _payload.size() & 0xff;
        payloadBase += 8;
    }
    else if(_payload.size() > 125) {
        buf[1] = 126;
        buf[2] = (_payload.size() >> 8) & 0xff;
        buf[3] = _payload.size() & 0xff;
        payloadBase += 2;
    } else {
        buf[1] = _payload.size();
    }
    memcpy(payloadBase, _payload.data(), _payload.size());
}

websocket_frame::~websocket_frame() {}

websocket::websocket(const shared_ptr<stream> &strm, bool deflate) :
        _strm(strm), _decoder(make_shared<websocket_frame::decoder>(0x100000)),
        _tx_zs(nullptr), _rx_zs(nullptr), _msg_deflated(false), _alive(true) {
    if (deflate) {
        _rx_zs = new z_stream;
        _rx_zs->zalloc = nullptr;
        _rx_zs->zfree = nullptr;
        _rx_zs->opaque = nullptr;
        if (inflateInit2(_rx_zs, -MAX_WBITS) != Z_OK) {
            delete _rx_zs;
            throw runtime_error("failed to initialize z_stream");
        }
        _tx_zs = new z_stream;
        _tx_zs->zalloc = nullptr;
        _tx_zs->zfree = nullptr;
        _tx_zs->opaque = nullptr;
        if (deflateInit2(_tx_zs, Z_BEST_COMPRESSION, Z_DEFLATED, -MAX_WBITS,
                         MAX_MEM_LEVEL, Z_DEFAULT_STRATEGY) != Z_OK) {
            delete _tx_zs;
            _tx_zs = nullptr;
        }
    }
}

bool websocket::poll() {
    if(_done)
        return true;
    while(_alive) {
        try {
            auto frame = _strm->read<websocket_frame>(_decoder);
            switch(frame->opcode()) {
                case 0: break;  // CONTINUATION
                case 1: case 2: // DATA OR TEXT
                    _reassembled.pull(_reassembled.size());
                    _msg_deflated = frame->deflated();
                    break;
                case 8: // CLOSE
                    cleanup();
                    continue;
                case 9: // PING
                    _strm->write(make_shared<websocket_frame>(10, frame->payload()));
                    continue;
            }
            if(frame->payload())
                _reassembled.append(frame->payload().data(), frame->payload().size());
            if(frame->fin()) {
                if(_msg_deflated && _reassembled.size() > 0) {
                    if(!_rx_zs)
                        throw runtime_error("message is deflated");
                    _reassembled.append("\0\0\xff\xff", 4);
                    _rx_zs->avail_in = _reassembled.size();
                    char *detached = _reassembled.detach();
                    _rx_zs->next_in = (Bytef *)detached;
                    while(_rx_zs->avail_in) {
                        _rx_zs->next_out = (Bytef *)_reassembled.prepare(XY_PAGESIZE);
                        _rx_zs->avail_out = XY_PAGESIZE;
                        int r = 0;
                        if((r = inflate(_rx_zs, Z_SYNC_FLUSH)) != Z_OK)
                            throw runtime_error("inflate failure");
                        _reassembled.commit(XY_PAGESIZE - _rx_zs->avail_out);
                    }
                    _msg_deflated = false;
                }
                // TODO: optimize stream_buffer::dump() to avoid copy
                _done = _reassembled.dump();
                _reassembled.pull(_reassembled.size());
                return true;
            }
        }
        catch(runtime_error &ex) {
            cleanup();
            throw;
        }
    }
    return false;
}

chunk websocket::read() {
    chunk result;
    if(_done || poll())
        result = move(_done);
    return result;
}

void websocket::send(const chunk &str) {
    if(_tx_zs && !str.empty()) {
        _tx_zs->next_in = (Bytef *)str.data();
        _tx_zs->avail_in = str.size();
        stream_buffer sb;
        while(_tx_zs->avail_in) {
            _tx_zs->next_out = (Bytef *)sb.prepare(XY_PAGESIZE);
            _tx_zs->avail_out = XY_PAGESIZE;
            if(deflate(_tx_zs, Z_SYNC_FLUSH) != Z_OK)
                throw runtime_error("deflate failure");
            sb.commit(XY_PAGESIZE - _tx_zs->avail_out);
        }
        _strm->write(make_shared<websocket_frame>(0x41, chunk(sb.data(), sb.size() - 4)));
    } else {
        _strm->write(make_shared<websocket_frame>(1, str));
    }
}

void websocket::cleanup() {
    _alive = false;
    _reassembled.pull(_reassembled.size());
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