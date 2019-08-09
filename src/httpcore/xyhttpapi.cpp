#include <iostream>
#include <cstring>
#include <unistd.h>

#include "xyhttp.h"
#include "xywebsocket.h"

#include <zlib.h>
#include <openssl/sha.h> // used by http_transaction::accept_websocket
#include <cassert>

const string http_transaction::SERVER_VERSION("xyhttpd/19.07 (xwsg.0xspot.com)");
const string http_transaction::WEBSOCKET_MAGIC("258EAFA5-E914-47DA-95CA-C5AB0DC85B11");

http_transaction::http_transaction(
    shared_ptr<http_connection> conn, shared_ptr<http_request> req) :
    connection(move(conn)), request(move(req)), _headerSent(false),
    _finished(false), _transfer_mode(UNDECIDED), _gzip(nullptr) {
    if(auto ctlen = request->header("content-length")) {
        int len = atoi(ctlen->c_str());
        if(len > 0x800000) {
            display_error(413);
            throw RTERR("request body too long (Content-Length=%d)", len);
        }
        char *buf = new char[len];
        char *bufpos = buf;
        auto dec = make_shared<rest_decoder>(len);
        while(dec->more()) {
            auto str = connection->_strm->read<string_message>(dec);
            memcpy(bufpos, str->data(), str->serialize_size());
            bufpos += str->serialize_size();
        }
        postdata = make_shared<string>(buf, len);
        delete[] buf;
    }
    _response = make_shared<http_response>(200);
    if(request->method() == HEAD)
        _transfer_mode = HEADONLY;
}

http_transaction::~http_transaction() {
    if(_gzip) {
        deflateEnd(_gzip);
        delete _gzip;
        _gzip = nullptr;
    }
}

void http_transaction::forward_to(const string &host, int port) {
    return forward_to(make_shared<ip_endpoint>(host, port));
}

void http_transaction::forward_to(shared_ptr<ip_endpoint> ep) {
    if(header_sent()) throw RTERR("header already sent");
    auto strm = make_shared<tcp_stream>();
    strm->connect(ep);
    auto req = make_shared<http_request>(*request);
    req->set_header("X-Forwarded-For", connection->_peername);
    req->delete_header("accept-encoding");
    strm->write(req);
    if(postdata) strm->write(*postdata);
    _response->set_code(100);
    auto respdec = make_shared<http_response::decoder>();
    while(_response->code() == 100) {
        _response = strm->read<http_response>(respdec);
        if(!_response) {
            display_error(502);
            return;
        }
    }
    if(auto contentLen = _response->header("Content-Length")) {
        int len = atoi(contentLen->c_str());
        auto dec = make_shared<rest_decoder>(len);
        while(dec->more()) {
            auto msg = strm->read<string_message>(dec);
            write(msg->data(), msg->serialize_size());
        }
    }
    else if (_response->code() == 101 && _response->header("Upgrade")) {
        auto client = upgrade();
        client->pipe(strm);
        strm->pipe(client);
        return;
    }
    else if (_response->code() != 304) {
        auto dec = make_shared<http_transfer_decoder>(
                _response->header("Transfer-Encoding"));
        _response->delete_header("Transfer-Encoding");
        while(true) {
            auto msg = strm->read<string_message>(dec);
            if(!msg) break;
            write(msg->data(), msg->serialize_size());
        }
    }
    finish();
}

void http_transaction::forward_to(shared_ptr<fcgi_connection> conn) {
    conn->set_env("PATH_INFO", request->path());
    conn->set_env("SERVER_PROTOCOL", "HTTP/1.1");
    conn->set_env("CONTENT_TYPE", request->header("content-type"));
    conn->set_env("CONTENT_LENGTH", request->header("content-length"));
    conn->set_env("SERVER_SOFTWARE", SERVER_VERSION);
    conn->set_env("REQUEST_URI", request->resource());
    if(request->query())
        conn->set_env("QUERY_STRING", request->query());
    conn->set_env("REQUEST_METHOD", request->method_name());
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
    _response = conn->read<http_response>(make_shared<http_response::decoder>());
    auto dec = make_shared<string_decoder>();
    while(true) {
        auto msg = conn->read<string_message>(dec);
        if(!msg) break;
        write(msg->data(), msg->serialize_size());
    }
    finish();
}

void http_transaction::serve_file(const string &filename) {
    if(request->method() != GET && request->method() != HEAD) {
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
    int tlen = strftime(ftbuf, sizeof(ftbuf),
        "%a, %d %b %Y %H:%M:%S GMT", gmtime(&info.st_mtime));
    string modtime(ftbuf, tlen);
    shared_ptr<string> chktime = request->header("if-modified-since");
    if(chktime && modtime == *chktime) {
        auto resp = get_response(304);
        finish();
        return;
    }
    _response->set_code(200);
    _response->set_header("Last-Modified", modtime);
    if(request->method() == HEAD) {
        _response->set_header("Content-Length", to_string(info.st_size));
        finish();
        return;
    }
    serve_file(filename, info);
}

void http_transaction::serve_file(const string &filename, struct stat &info) {
    auto range = request->header("range");
    size_t seekTo = 0, rest = info.st_size;
    if(range && memcmp(range->data(), "bytes=", 6) == 0) {
        string byteRange = range->substr(6);
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
    size_t chunkSize = max<size_t>(info.st_blksize, 8192);
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
    if(_gzip) {
        deflateEnd(_gzip);
        delete _gzip;
        _gzip = nullptr;
    }
    _transfer_mode = UPGRADE;
    if(flush_resp) {
        flush_response();
    } else {
        _headerSent = true;
    }
    _finished = true;
    return connection->upgrade();
}

shared_ptr<websocket> http_transaction::accept_websocket() {
    if(request->method() != GET) {
        display_error(405);
        throw RTERR("WebSocket negotiation expects GET, got %s",
                    request->method_name());
    }
    auto wskey = request->header("sec-websocket-key");
    if(!request->header("upgrade") || !wskey)
        throw RTERR("Headers necessary for WebSocket handshake not present");
    string wsaccept = *wskey + WEBSOCKET_MAGIC;
    unsigned char shabuf[SHA_DIGEST_LENGTH];
    SHA1((unsigned char *)wsaccept.data(), wsaccept.size(), shabuf);
    auto resp = get_response(101);
    bool permsgDeflate;
    resp->set_header("Upgrade", "websocket");
    resp->set_header("Sec-WebSocket-Accept",
            base64_encode(shabuf, SHA_DIGEST_LENGTH));
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
    resp->set_header("Content-Type", "text/html");
    write(fmt("<html><head><title>XWSG Error %d</title></head>"
              "<body><h1>%d %s</h1></body></html>", code, code,
              http_response::state_description(code)));
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

void http_transaction::flush_response() {
    assert(!_headerSent);
    _response->set_header("Server", SERVER_VERSION);
    if(_transfer_mode == UPGRADE) {
        _response->set_header("Connection", "upgrade");
    } else if(_transfer_mode == CHUNKED) {
        _response->delete_header("Content-Length");
        _response->set_header("Transfer-Encoding", "chunked");
    } else {
        _response->set_header("Connection",
                              connection->keep_alive() ? "keep-alive" : "close");
    }
    connection->_strm->write(_response);
    _headerSent = true;
}

void http_transaction::write(const char *buf, int len) {
    if(_finished)
        throw RTERR("writing to finished transaction");
    if(_gzip) {
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
        if(len == 0) return;
        transfer(buf, len);
        if(_transfer_mode == UNDECIDED && _tx_buffer.size() > 0x100 &&
            request->header_include("accept-encoding", "gzip")) {
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
        case UNDECIDED:
            _tx_buffer.append(buf, len);
            if(_tx_buffer.size() > 0x20000) {
                _transfer_mode = CHUNKED;
                flush_response();
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
    if(_response->code() == 304 || _transfer_mode == HEADONLY)
        _finished = true;
    if(_finished) {
        if(!_headerSent) flush_response();
        return;
    }
    if(_gzip) {
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
    if(_transfer_mode == UNDECIDED) {
        _transfer_mode = SIMPLE;
        _response->set_header("Content-Length", to_string(_tx_buffer.size()));
        flush_response();
        if(_tx_buffer.size() > 0) {
            connection->_strm->write(_tx_buffer.data(), _tx_buffer.size());
            _tx_buffer.pull(_tx_buffer.size());
        }
    }
    else if(_transfer_mode == CHUNKED)
        connection->_strm->write("0\r\n\r\n", 5);
    _finished = true;
}