#include <iostream>
#include <cstring>

#include "xyhttp.h"

#define CURRENT_ISUPPER (chunk[i] >= 'A' && chunk[i] <= 'Z')
#define CURRENT_ISLOWER (chunk[i] >= 'a' && chunk[i] <= 'z')
#define CURRENT_ISNUMBER (chunk[i] >= '0' && chunk[i] <= '9')
#define CURRENT_VALUE chunk + currentBase, i - currentBase

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
        case 300: return "Multiple Choices";
        case 301: return "Moved Permanently";
        case 302: return "Found";
        case 303: return "See Other";
        case 304: return "Not Modified";
        case 400: return "Bad Request";
        case 401: return "Unauthorized";
        case 403: return "Fobidden";
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
        case 418: return "I'm a teapot";
        case 426: return "Upgrade Required";
        case 500: return "Internal Server Error";
        case 501: return "Not Implemented";
        case 502: return "Bad Gateway";
        case 503: return "Service Unavailable";
        default: return NULL;
    }
}

http_response::http_response(int c) : code(c) {}

int http_response::serialize_size() {
    int size = 15 + strlen(state_description(code));
    for(auto it = _headers.begin(); it != _headers.end(); it++)
        size += 4 + it->first.size() + it->second->size();
    return size + 2;
}

void http_response::serialize(char *buf) {
    buf += sprintf(buf, "HTTP/1.1 %d %s\r\n", code, state_description(code));
    for(auto it = _headers.begin(); it != _headers.end(); it++)
        buf += sprintf(buf, "%s: %s\r\n",
            it->first.c_str(), it->second->c_str());
    memcpy(buf, "\r\n", 2);
}

int http_response::type() const {
    return XY_MESSAGE_RESP;
}

http_response::~http_response() {}

http_response::decoder::decoder() {}

bool http_response::decoder::decode(shared_ptr<streambuffer> &stb) {
    shared_ptr<http_response> resp;
    const char *chunk = stb->data();
    int i = 0, currentExpect = 0, currentBase = 0;
    int verbOrKeyLength;
    char headerKey[32];
    if(stb->size() > 0x10000)
        throw runtime_error("request too long");
    while(i < stb->size()) {
        switch(currentExpect) {
            case 0: // expect HTTP version - HTTP/1.
                if(chunk[i] == '.') {
                    if(i - currentBase != 6 ||
                       chunk[currentBase + 0] != 'H' ||
                       chunk[currentBase + 1] != 'T' ||
                       chunk[currentBase + 2] != 'T' ||
                       chunk[currentBase + 3] != 'P' ||
                       chunk[currentBase + 4] != '/' ||
                       chunk[currentBase + 5] != '1') {
                        goto on_malformed_header;
                    }
                    currentBase = i + 1;
                    currentExpect = 1;
                }
                else if(!CURRENT_ISUPPER &&
                        chunk[i] != '/' && chunk[i] != '1')
                    goto on_malformed_header;
                break;
            case 1: // expect HTTP subversion
                if(chunk[i] == ' ') {
                    currentBase = i + 1;
                    currentExpect = 2;
                }
                else if((chunk[i] != '0' && chunk[i] != '1') ||
                        i != currentBase)
                    goto on_malformed_header;
                break;
            case 2: // expect HTTP status code
                if(chunk[i] == ' ') {
                    resp = shared_ptr<http_response>(
                        new http_response(atoi(chunk + currentBase)));
                    currentBase = i + 1;
                    currentExpect = 3;
                }
                else if(!CURRENT_ISNUMBER)
                    goto on_malformed_header;
                break;
            case 3: // skip HTTP status text
                if(chunk[i] == '\n') {
                    currentBase = i + 1;
                    currentExpect = 4;
                }
                else if(chunk[i] == '\r')
                    currentExpect = 100;
                break;
            case 4: // expect HTTP header key
                if(chunk[i] == ':') {
                    verbOrKeyLength = i - currentBase;
                    currentExpect = 5;
                }
                else if(i == currentBase && chunk[i] == '\n')
                    goto entire_request_decoded;
                else if(i == currentBase && chunk[i] == '\r')
                    currentExpect = 101;
                else if(i - currentBase > 30)
                    goto on_malformed_header;
                else if(CURRENT_ISLOWER || chunk[i] == '-' || chunk[i] == '_' ||
                        CURRENT_ISNUMBER || CURRENT_ISUPPER)
                    headerKey[i - currentBase] = chunk[i];
                else
                    goto on_malformed_header;
                break;
            case 5: // skip spaces between column and value
                if(chunk[i] != ' ') {
                    currentBase = i;
                    currentExpect = 6;
                }
                break;
            case 6: // expect HTTP header value
                if(!(chunk[i] >= 0 && chunk[i] < 32))
                    break;
                else if(chunk[i] == '\n' || chunk[i] == '\r') {
                    resp->_headers[string(headerKey, verbOrKeyLength)]
                            = shared_ptr<string>(new string(CURRENT_VALUE));
                    currentBase = i + 1;
                    currentExpect = chunk[i] == '\r' ? 100 : 4;
                    break;
                }
                goto on_malformed_header;
            case 100: // expect \n after '\r'
                if(chunk[i] != '\n') goto on_malformed_header;
                currentBase = i + 1;
                currentExpect = 4;
                break;
            case 101: // expect final \n
                if(chunk[i] != '\n') goto on_malformed_header;
                goto entire_request_decoded;
        }
        i++;
    }
    return false;
    on_malformed_header:
    throw runtime_error("malformed response");
    entire_request_decoded:
    _msg = resp;
    stb->pull(i + 1);
    return true;
}

shared_ptr<message> http_response::decoder::msg() {
    return _msg;
}

http_response::decoder::~decoder() {}