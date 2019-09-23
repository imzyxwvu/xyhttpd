#include <iostream>
#include <cstring>

#include "xyhttp.h"

using namespace std;

#define CURRENT_ISUPPER (stb[i] >= 'A' && stb[i] <= 'Z')
#define CURRENT_ISLOWER (stb[i] >= 'a' && stb[i] <= 'z')
#define CURRENT_ISNUMBER (stb[i] >= '0' && stb[i] <= '9')
#define CURRENT_VALUE stb.data() + currentBase, i - currentBase

int http_request::serialize_size() {
    int size = 12 + method.size() + _resource.size();
    for(auto it = _headers.begin(); it != _headers.end(); it++)
        size += 4 + it->first.size() + it->second.size();
    return size + 2;
}

void http_request::serialize(char *buf) {
    buf += sprintf(buf, "%s %s HTTP/1.1\r\n", method.c_str(), _resource.data());
    for(auto it = _headers.begin(); it != _headers.end(); it++)
        buf += sprintf(buf, "%s: %s\r\n", it->first.c_str(), it->second.data());
    memcpy(buf, "\r\n", 2);
}

http_request::~http_request() {}

bool http_request::decoder::decode(stream_buffer &stb) {
    auto req = make_shared<http_request>();
    int i = 0, currentExpect = 0, currentBase;
    int verbOrKeyLength;
    char headerKey[32];
    if(stb.size() > 0x10000)
        throw runtime_error("request too long");
    while(i < stb.size()) {
        switch(currentExpect) {
            case 0: // expect HTTP method
                if(stb[i] == ' ') {
                    verbOrKeyLength = i;
                    if(verbOrKeyLength > 20)
                        throw runtime_error("malformed request");
                    currentBase = i + 1;
                    currentExpect = 1;
                }
                else if(!CURRENT_ISUPPER && stb[i] != '_')
                    throw runtime_error("malformed request");
                break;
            case 1: // expect resource
                if(stb[i] == ' ') {
                    req->method = string(stb.data(), verbOrKeyLength);
                    req->set_resource(chunk(CURRENT_VALUE));
                    currentBase = i + 1;
                    currentExpect = 2;
                }
                else if(stb[i] >= 0 && stb[i] < 32)
                    throw runtime_error("malformed request");
                break;
            case 2: // expect HTTP version - HTTP/1.
                if(stb[i] == '.') {
                    if(i - currentBase != 6 ||
                       memcmp(stb.data() + currentBase, "HTTP/1", 6) != 0) {
                        throw runtime_error("malformed request");
                    }
                    currentBase = i + 1;
                    currentExpect = 3;
                }
                else if(!CURRENT_ISUPPER && stb[i] != '/' && stb[i] != '1') {
                    throw runtime_error("malformed request");
                }
                break;
            case 3: // expect HTTP subversion
                if(stb[i] == '\n') {
                    currentBase = i + 1;
                    currentExpect = 4;
                }
                else if(stb[i] == '\r')
                    currentExpect = 100;
                else if((stb[i] != '0' && stb[i] != '1') ||
                        i != currentBase)
                    throw runtime_error("malformed request");
                break;
            case 4: // expect HTTP header key
                if(stb[i] == ':') {
                    verbOrKeyLength = i - currentBase;
                    currentExpect = 5;
                }
                else if(i == currentBase && stb[i] == '\n')
                    goto entire_request_decoded;
                else if(i == currentBase && stb[i] == '\r')
                    currentExpect = 101;
                else if(i - currentBase > 30)
                    throw runtime_error("malformed request");
                else if(CURRENT_ISUPPER)
                    headerKey[i - currentBase] = stb[i] + 32;
                else if(CURRENT_ISLOWER || stb[i] == '-' || stb[i] == '_' || CURRENT_ISNUMBER)
                    headerKey[i - currentBase] = stb[i];
                else
                    throw runtime_error("malformed request");
                break;
            case 5: // skip spaces between column and value
                if(stb[i] != ' ') {
                    currentBase = i;
                    currentExpect = 6;
                }
                break;
            case 6: // expect HTTP header value
                if(!(stb[i] >= 0 && stb[i] < 32))
                    break;
                else if(stb[i] == '\n' || stb[i] == '\r') {
                    req->_headers[string(headerKey, verbOrKeyLength)]
                            = chunk(CURRENT_VALUE);
                    currentBase = i + 1;
                    currentExpect = stb[i] == '\r' ? 100 : 4;
                    break;
                }
                throw runtime_error("malformed request");
            case 100: // expect \n after '\r'
                if(stb[i] != '\n') throw runtime_error("malformed request");
                currentBase = i + 1;
                currentExpect = 4;
                break;
            case 101: // expect final \n
                if(stb[i] != '\n') throw runtime_error("malformed request");
                goto entire_request_decoded;
        }
        i++;
    }
    return false;
    entire_request_decoded:
    _msg = req;
    stb.pull(i + 1);
    return true;
}

void http_request::set_resource(chunk res) {
    _resource = move(res);
    const char *queryBase = strchr(_resource.data(), '?');
    if(queryBase) {
        _path = chunk(_resource.data(), queryBase - _resource.data());
        _query = chunk(queryBase + 1);
    } else {
        _path = _resource;
        _query = nullptr;
    }
}

void http_request::delete_header(const string &key) {
    _headers.erase(key);
}

bool http_request::header_include(const string &key, const string &kw) {
    auto it = _headers.find(key);
    if(it == _headers.end()) return false;
    return it->second.find(kw.c_str()) != -1;
}

unordered_map<string, chunk>::const_iterator http_request::hbegin() const {
    return _headers.cbegin();
}

unordered_map<string, chunk>::const_iterator http_request::hend() const {
    return _headers.cend();
}

http_request::decoder::~decoder() {}
const char* http_response::state_description(int code) {
    switch(code) {
        case 100: return "Continue";
        case 101: return "Switching Protocols";
        case 200: return "OK";
        case 201: return "Created";
        case 202: return "Accepted";
        case 203: return "Non-Authoritative Information";
        case 204: return "No Content";
        case 205: return "Reset Content";
        case 206: return "Partial Content";
        case 207: return "Multi-Status"; // RFC 2518 - WebDAV
        case 300: return "Multiple Choices";
        case 301: return "Moved Permanently";
        case 302: return "Found";
        case 303: return "See Other";
        case 304: return "Not Modified";
        case 400: return "Bad Request";
        case 401: return "Unauthorized";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 406: return "Not Acceptable";
        case 408: return "Request Time-out";
        case 409: return "Conflict";
        case 410: return "Gone";
        case 411: return "Length Required";
        case 412: return "Precondition Failed";
        case 413: return "Request Entity Too Large";
        case 415: return "Unsupported Media Type";
        case 416: return "Requested Range Not Satisfiable";
        case 417: return "Expectation Failed";
        case 418: return "I'm a teapot"; // RFC 2324 - HTCPCP
        case 426: return "Upgrade Required";
        case 500: return "Internal Server Error";
        case 501: return "Not Implemented";
        case 502: return "Bad Gateway";
        case 503: return "Service Unavailable";
        default: return nullptr;
    }
}

http_response::http_response(int c) : _code(c) {}

int http_response::serialize_size() {
    int size = 15 + strlen(state_description(_code));
    for(auto it = _headers.begin(); it != _headers.end(); it++)
        if(it->second)
            size += 4 + it->first.size() + it->second.size();
    for(auto it = cookies.begin(); it != cookies.end(); it++)
        size += 14 + it->size();
    return size + 2;
}

void http_response::serialize(char *buf) {
    buf += sprintf(buf, "HTTP/1.1 %d %s\r\n", _code, state_description(_code));
    for(auto it = _headers.begin(); it != _headers.end(); it++)
        buf += sprintf(buf, "%s: %s\r\n", it->first.c_str(), it->second.data());
    for(auto it = cookies.begin(); it != cookies.end(); it++)
        buf += sprintf(buf, "Set-Cookie: %s\r\n", it->data());
    memcpy(buf, "\r\n", 2);
}

void http_response::set_code(int newcode) {
    if(!state_description(newcode))
        throw RTERR("invalid HTTP response code - %d", newcode);
    _code = newcode;
}

void http_response::set_header(const string &key, chunk val) {
    if(!val) return;
    if(key == "Set-Cookie") {
        cookies.push_back(val);
    } else if(key == "Status") {
        _code = atoi(val.data());
    } else {
        _headers[key] = val;
    }
}

void http_response::delete_header(const string &key) {
    _headers.erase(key);
}

http_response::~http_response() {}

bool http_response::decoder::decode(stream_buffer &stb) {
    shared_ptr<http_response> resp;
    int i = 0, currentExpect = 0, currentBase = 0;
    int verbOrKeyLength;
    char headerKey[32];
    if(stb.size() > 0x10000)
        throw runtime_error("request too long");
    while(i < stb.size()) {
        switch(currentExpect) {
            case 0: // expect HTTP version - HTTP/1.
                if(stb[i] == '.') {
                    if(i - currentBase != 6 ||
                       headerKey[0] != 'H' || headerKey[1] != 'T' ||
                       headerKey[2] != 'T' || headerKey[3] != 'P' ||
                       headerKey[4] != '/' || headerKey[5] != '1') {
                        throw runtime_error("malformed response");
                    }
                    currentBase = i + 1;
                    currentExpect = 1;
                }
                else if(stb[i] == ':') {
                    resp = make_shared<http_response>(200);
                    verbOrKeyLength = i - currentBase;
                    currentExpect = 5;
                }
                else if(CURRENT_ISUPPER || CURRENT_ISLOWER || CURRENT_ISNUMBER ||
                        stb[i] == '-' || stb[i] == '_' || stb[i] == '/')
                    headerKey[i - currentBase] = stb[i];
                else if(stb[i] != '/')
                    throw runtime_error("malformed response");
                break;
            case 1: // expect HTTP subversion
                if(stb[i] == ' ') {
                    currentBase = i + 1;
                    currentExpect = 2;
                }
                else if((stb[i] != '0' && stb[i] != '1') ||
                        i != currentBase)
                    throw runtime_error("malformed response");
                break;
            case 2: // expect HTTP status code
                if(stb[i] == ' ') {
                    resp = make_shared<http_response>(atoi(stb.data() + currentBase));
                    currentBase = i + 1;
                    currentExpect = 3;
                }
                else if(!CURRENT_ISNUMBER)
                    throw runtime_error("malformed response");
                break;
            case 3: // skip HTTP status text
                if(stb[i] == '\n') {
                    currentBase = i + 1;
                    currentExpect = 4;
                }
                else if(stb[i] == '\r')
                    currentExpect = 100;
                break;
            case 4: // expect HTTP header key
                if(stb[i] == ':') {
                    verbOrKeyLength = i - currentBase;
                    currentExpect = 5;
                }
                else if(i == currentBase && stb[i] == '\n')
                    goto entire_request_decoded;
                else if(i == currentBase && stb[i] == '\r')
                    currentExpect = 101;
                else if(i - currentBase > 30)
                    throw runtime_error("malformed response");
                else if(CURRENT_ISLOWER || stb[i] == '-' || stb[i] == '_' ||
                        CURRENT_ISNUMBER || CURRENT_ISUPPER)
                    headerKey[i - currentBase] = stb[i];
                else
                    throw runtime_error("malformed response");
                break;
            case 5: // skip spaces between column and value
                if(stb[i] != ' ') {
                    currentBase = i;
                    currentExpect = 6;
                }
                break;
            case 6: // expect HTTP header value
                if(!(stb[i] >= 0 && stb[i] < 32))
                    break;
                else if(stb[i] == '\n' || stb[i] == '\r') {
                    resp->set_header(
                            string(headerKey, verbOrKeyLength),
                            chunk(stb.data() + currentBase, i - currentBase));
                    currentBase = i + 1;
                    currentExpect = stb[i] == '\r' ? 100 : 4;
                    break;
                }
                throw runtime_error("malformed response");
            case 100: // expect \n after '\r'
                if(stb[i] != '\n') throw runtime_error("malformed response");
                currentBase = i + 1;
                currentExpect = 4;
                break;
            case 101: // expect final \n
                if(stb[i] != '\n') throw runtime_error("malformed response");
                goto entire_request_decoded;
        }
        i++;
    }
    return false;
    entire_request_decoded:
    _msg = resp;
    stb.pull(i + 1);
    return true;
}

http_response::decoder::~decoder() {}

http_transfer_decoder::http_transfer_decoder(const P<http_response> &resp) {
    auto transferEnc = resp->header("Transfer-Encoding");
    _chunked = transferEnc && transferEnc.find("chunked") != -1;
    if(!_chunked) {
        auto contentLen = resp->header("Content-Length");
        _restBytes = contentLen ? atoi(contentLen.data()) : -1;
    }
}

bool http_transfer_decoder::decode(stream_buffer &stb) {
    if(_restBytes == -1 && _chunked) {
        if(stb.size() > 3) {
            char *end;
            int len = strtol(stb.data(), &end, 16);
            int chunkSize = end - stb.data();
            if(!(chunkSize > 0 && end[0] == '\r' && end[1] == '\n'))
                throw runtime_error("bad chunked protocol");
            chunkSize += len + 4;
            if(stb.size() < chunkSize) return false;
            _nBytes = len;
            if(_nBytes == 0) {
                _restBytes = 0; // mark this over
                stb.pull(chunkSize);
                _msg = make_shared<string_message>(nullptr, 0);
                return true;
            }
            char *buffer = (char *)malloc(_nBytes);
            memcpy(buffer, end + 2, _nBytes);
            _msg = make_shared<string_message>(buffer, _nBytes);
            stb.pull(chunkSize);
            return true;
        }
        return false;
    } else {
        return string_decoder::decode(stb);
    }
}