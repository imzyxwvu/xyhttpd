#include <iostream>
#include <cstring>
#include <unistd.h>

#include "xyhttp.h"

const string http_transaction::SERVER_VERSION("XWSG/18.12 (xwsg.xuejietech.cn)");

http_transaction::http_transaction(
    shared_ptr<http_connection> conn, shared_ptr<http_request> req) :
    connection(conn), request(req), _header_sent(false) {
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
}

void http_transaction::forward_to(const string &host, int port) {
    shared_ptr<ip_endpoint> ep(new ip_endpoint(host, port));
    return forward_to(ep);
}

void http_transaction::forward_to(shared_ptr<ip_endpoint> ep) {
    if(_header_sent)
        throw RTERR("header already sent");
    shared_ptr<tcp_stream> strm(new tcp_stream);
    strm->connect(ep);
    shared_ptr<http_request> newreq(new http_request(*request));
    newreq->set_header("X-Forwarded-For", connection->_peername);
    strm->write(newreq);
    if(postdata) strm->write(*postdata);
    shared_ptr<http_response::decoder> respdec(new http_response::decoder());
    while(!_response || _response->code() == 100)
        _response = strm->read<http_response>(respdec);
    if(auto len = _response->header("Content-Length")) {
        auto dec = shared_ptr<rest_decoder>(
                new rest_decoder(atoi(len->c_str())));
        while(dec->more()) {
            auto msg = strm->read<string_message>(dec);
            write(msg->data(), msg->serialize_size());
        }
    }
    else if (_response->code() == 304)
        flush_response();
    else {
        connection->_keep_alive = false;
        _response->set_header("Connection", "close");
        auto dec = shared_ptr<string_decoder>(new string_decoder());
        while(true) {
            auto msg = strm->read<string_message>(dec);
            if(!msg) break;
            write(msg->data(), msg->serialize_size());
        }
    }
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
    connection->_keep_alive = false;
    _response->set_header("Connection", "close");
    flush_response();
    auto dec = shared_ptr<string_decoder>(new string_decoder());
    while(true) {
        auto msg = conn->read<string_message>(dec);
        if(!msg) break;
        write(msg->data(), msg->serialize_size());
    }
}

void http_transaction::serve_file(const string &filename) {
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
        auto resp = make_response(304);
        flush_response();
        return;
    }
    int fd = open(filename.c_str(), O_RDONLY);
    if(fd < 0) {
        display_error(403);
        return;
    }
    auto resp = make_response(200);
    resp->set_header("Last-Modified", modtime);
    resp->set_header("Content-Length", to_string(info.st_size));
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
}

void http_transaction::redirect_to(const string &dest) {
    auto resp = make_response(302);
    resp->set_header("Location", dest);
    resp->set_header("Content-Length", "0");
    flush_response();
}

void http_transaction::display_error(int code) {
    auto resp = make_response(code);
    char page[256];
    int size = sprintf(page, "<html><head><title>XWSG Error %d</title></head>"
        "<body><h1>%d %s</h1></body></html>", code, code,
        http_response::state_description(code));
    resp->set_header("Content-Type", "text/html");
    resp->set_header("Content-Length", to_string(size));
    write(page, size);
}

shared_ptr<http_response> http_transaction::make_response() {
    if(_header_sent)
        throw runtime_error("header already sent");
    if(!_response) {
        _response = shared_ptr<http_response>(new http_response(200));
    }
    return _response;
}

shared_ptr<http_response> http_transaction::make_response(int code) {
    if(_header_sent)
        throw runtime_error("header already sent");
    if(_response) {
        _response->set_code(code);
    } else {
        _response = shared_ptr<http_response>(new http_response(code));
    }
    return _response;
}

void http_transaction::flush_response() {
    if(_header_sent) return;
    if(!_response) make_response();
    _response->set_header("Server", SERVER_VERSION);
    _response->set_header("Connection",
        connection->keep_alive() ? "keep-alive" : "close");
    connection->_strm->write(_response);
    _header_sent = true;
}

void http_transaction::write(const char *buf, int len) {
    flush_response();
    connection->_strm->write(buf, len);
}

void http_transaction::write(const string &buf) {
    write(buf.data(), buf.size());
}