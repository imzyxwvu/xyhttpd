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
    connection(conn), request(req), _headerSent(false),
    _finished(false), _transfer_mode(UNDECIDED) {
    if(auto ctlen = request->header("content-length")) {
        int len = atoi(ctlen->c_str());
        if(len > 0x800000) {
            display_error(413);
            throw RTERR("request body too long (Content-Length=%d)", len);
        }
        char *buf = new char[len];
        char *bufpos = buf;
        shared_ptr<rest_decoder> dec(new rest_decoder(len));
        while(dec->more()) {
            auto str = connection->_strm->read<string_message>(dec);
            memcpy(bufpos, str->data(), str->serialize_size());
            bufpos += str->serialize_size();
        }
        postdata = shared_ptr<string>(new string(buf, len));
        delete buf;
    }
    _response = make_shared<http_response>(200);
}

void http_transaction::forward_to(const string &host, int port) {
    shared_ptr<ip_endpoint> ep(new ip_endpoint(host, port));
    return forward_to(ep);
}

void http_transaction::forward_to(shared_ptr<ip_endpoint> ep) {
    if(header_sent()) throw RTERR("header already sent");
    shared_ptr<tcp_stream> strm(new tcp_stream);
    strm->connect(ep);
    shared_ptr<http_request> newreq(new http_request(*request));
    newreq->set_header("X-Forwarded-For", connection->_peername);
    newreq->delete_header("accept-encoding");
    strm->write(newreq);
    if(postdata) strm->write(*postdata);
    _response->set_code(100);
    shared_ptr<http_response::decoder> respdec(new http_response::decoder());
    while(_response->code() == 100)
        _response = strm->read<http_response>(respdec);
    if(auto contentLen = _response->header("Content-Length")) {
        int len = atoi(contentLen->c_str());
        declare_length(len);
        auto dec = shared_ptr<rest_decoder>(new rest_decoder(len));
        while(dec->more()) {
            auto msg = strm->read<string_message>(dec);
            write(msg->data(), msg->serialize_size());
        }
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
    auto dec = shared_ptr<string_decoder>(new string_decoder());
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
    auto resp = get_response(200);
    resp->set_header("Last-Modified", modtime);
    declare_length(info.st_size);
    if(request->method() == HEAD) {
        finish();
        return;
    }
    int fd = open(filename.c_str(), O_RDONLY);
    if(fd < 0) {
        display_error(403);
        return;
    }
    char *buf = new char[info.st_blksize];
    size_t rest = info.st_size;
    while(rest > 0) {
        size_t avail = read(fd, buf, info.st_blksize);
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

shared_ptr<stream> http_transaction::upgrade() {
    _transfer_mode = UPGRADE;
    flush_response();
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
    resp->set_header("Upgrade", "websocket");
    resp->set_header("Sec-WebSocket-Accept",
            base64_encode(shabuf, SHA_DIGEST_LENGTH));
    return make_shared<websocket>(upgrade());
}

void http_transaction::redirect_to(const string &dest) {
    auto resp = get_response(302);
    resp->set_header("Location", dest);
    flush_response();
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
        _response->set_header("Transfer-Encoding", "chunked");
    } else {
        _response->set_header("Connection",
                              connection->keep_alive() ? "keep-alive" : "close");
    }
    connection->_strm->write(_response);
    _headerSent = true;
}

void http_transaction::declare_length(int len) {
    if(_transfer_mode == UNDECIDED && len < 0x80000 &&
            len > 0x200 && request->method() != HEAD) {
        if(request->header_include("accept-encoding", "deflate")) {
            _response->set_header("Content-Encoding", "deflate");
            _transfer_mode = GZIP;
        }
    }
    if(_transfer_mode == UNDECIDED) {
        _transfer_mode = SIMPLE;
        _response->set_header("Content-Length", to_string(len));
    }
}

void http_transaction::write(const char *buf, int len) {
    if(_finished)
        throw RTERR("HTTP transaction already finished");
    if(len == 0) return;
    switch(_transfer_mode) {
        case UNDECIDED:
            _tx_buffer.append(buf, len);
            if(_tx_buffer.size() > 0x80000) {
                _transfer_mode = CHUNKED;
                flush_response();
                write_chunk(_tx_buffer.data(), _tx_buffer.size());
                _tx_buffer.pull(_tx_buffer.size());
            }
            break;
        case GZIP:
            _tx_buffer.append(buf, len);
            break;
        case SIMPLE:
            if(!_headerSent) flush_response();
            connection->_strm->write(buf, len);
            break;
        case CHUNKED:
            write_chunk(buf, len);
            break;
    }
}

void http_transaction::write_chunk(const char *buf, int len) {
    char chunkHdr[16];
    int hdrLen = sprintf(chunkHdr, "%x\r\n", len);
    connection->_strm->write(chunkHdr, hdrLen);
    connection->_strm->write(buf, len);
    connection->_strm->write("\r\n", 2);

}

void http_transaction::write(const string &buf) {
    write(buf.data(), buf.size());
}

void http_transaction::finish() {
    if(_transfer_mode == SIMPLE || _response->code() == 304)
        _finished = true;
    if(_finished) {
        if(!_headerSent) flush_response();
        return;
    }
    if(_transfer_mode == UNDECIDED) declare_length(_tx_buffer.size());
    switch(_transfer_mode) {
        case GZIP: {
            unsigned long bound = compressBound(_tx_buffer.size());
            char *buf = new char[bound];
            if(compress((Bytef *)buf, &bound,
                    (Bytef *)_tx_buffer.data(), _tx_buffer.size()) == Z_OK
                    && bound < _tx_buffer.size()) {
                _tx_buffer.pull(_tx_buffer.size());
                _response->set_header("Content-Length", to_string(bound));
                flush_response();
                connection->_strm->write(buf, bound);
                delete[] buf;
            } else {
                delete[] buf;
                _response->set_header("Content-Length", to_string(_tx_buffer.size()));
                _response->delete_header("Content-Encoding");
                flush_response();
                connection->_strm->write(_tx_buffer.data(), _tx_buffer.size());
                _tx_buffer.pull(_tx_buffer.size());
            }
            break;
        }
        case CHUNKED:
            connection->_strm->write("0\r\n\r\n", 5);
            break;
        case SIMPLE:
            flush_response();
            if(_tx_buffer.size() > 0) {
                connection->_strm->write(_tx_buffer.data(), _tx_buffer.size());
                _tx_buffer.pull(_tx_buffer.size());
            }
            break;
    }
    _finished = true;
}