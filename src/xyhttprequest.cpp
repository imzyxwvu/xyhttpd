#include <iostream>
#include <cstring>

#include "xyhttp.h"

#define CURRENT_ISUPPER (chunk[i] >= 'A' && chunk[i] <= 'Z')
#define CURRENT_ISLOWER (chunk[i] >= 'a' && chunk[i] <= 'z')
#define CURRENT_ISNUMBER (chunk[i] >= '0' && chunk[i] <= '9')
#define CURRENT_VALUE chunk + currentBase, i - currentBase

http_request::http_request() {}

void http_request::set_method(const char *method) {
    if(strcmp(method, "GET") == 0) _meth = GET;
    else if(strcmp(method, "POST") == 0) _meth = POST;
    else if(strcmp(method, "HEAD") == 0) _meth = HEAD;
    else if(strcmp(method, "PUT") == 0) _meth = PUT;
    else if(strcmp(method, "DELETE") == 0) _meth = DELETE;
    else if(strcmp(method, "OPTIONS") == 0) _meth = OPTIONS;
    else if(strcmp(method, "CONNECT") == 0) _meth = CONNECT;
    else if(strcmp(method, "BREW") == 0) _meth = BREW;
    else if(strcmp(method, "M-SEARCH") == 0) _meth = M_SEARCH;
    else
        throw invalid_argument("unsupported method");
}

const char *http_request::method_name() {
    switch(_meth) {
    case GET: return "GET";
    case POST: return "POST";
    case HEAD: return "HEAD";
    case PUT: return "PUT";
    case DELETE: return "DELETE";
    case OPTIONS: return "OPTIONS";
    case CONNECT: return "CONNECT";
    case BREW: return "BREW";
    default:
        throw logic_error("invalid method");
    }
}

int http_request::type() const {
    return XY_MESSAGE_REQUEST;
}

int http_request::serialize_size() {
    int size = 12 + strlen(method_name()) + _resource->size();
    for(auto it = _headers.begin(); it != _headers.end(); it++)
        size += 4 + it->first.size() + it->second->size();
    return size + 2;
}

void http_request::serialize(char *buf) {
    buf += sprintf(buf, "%s %s HTTP/1.1\r\n",
        method_name(), _resource->c_str());
    for(auto it = _headers.begin(); it != _headers.end(); it++)
        buf += sprintf(buf, "%s: %s\r\n",
            it->first.c_str(), it->second->c_str());
    memcpy(buf, "\r\n", 2);
}

http_request::~http_request() {}

http_request::decoder::decoder() {}

bool http_request::decoder::decode(shared_ptr<streambuffer> &stb) {
    shared_ptr<http_request> req(new http_request());
    const char *chunk = stb->data();
    int i = 0, currentExpect = 0, currentBase;
    int verbOrKeyLength;
    char headerKey[32];
    if(stb->size() > 0x10000)
        throw runtime_error("request too long");
    while(i < stb->size()) {
        switch(currentExpect) {
            case 0: // expect HTTP method
                if(chunk[i] == ' ') {
                    verbOrKeyLength = i;
                    if(verbOrKeyLength > 20)
                        throw runtime_error("malformed request");
                    currentBase = i + 1;
                    currentExpect = 1;
                }
                else if(!CURRENT_ISUPPER)
                    throw runtime_error("malformed request");
                break;
            case 1: // expect resource
                if(chunk[i] == ' ') {
                    char method[32];
                    memcpy(method, chunk, verbOrKeyLength);
                    method[verbOrKeyLength] = 0;
                    req->set_method(method);
                    req->_resource = shared_ptr<string>(new string(CURRENT_VALUE));
                    currentBase = i + 1;
                    currentExpect = 2;
                }
                else if(chunk[i] >= 0 && chunk[i] < 32)
                    throw runtime_error("malformed request");
                break;
            case 2: // expect HTTP version - HTTP/1.
                if(chunk[i] == '.') {
                    if(i - currentBase != 6 ||
                       chunk[currentBase + 0] != 'H' ||
                       chunk[currentBase + 1] != 'T' ||
                       chunk[currentBase + 2] != 'T' ||
                       chunk[currentBase + 3] != 'P' ||
                       chunk[currentBase + 4] != '/' ||
                       chunk[currentBase + 5] != '1') {
                        throw runtime_error("malformed request");
                    }
                    currentBase = i + 1;
                    currentExpect = 3;
                }
                else if(!CURRENT_ISUPPER &&
                        chunk[i] != '/' && chunk[i] != '1') {
                    throw runtime_error("malformed request");
                }
                break;
            case 3: // expect HTTP subversion
                if(chunk[i] == '\n') {
                    currentBase = i + 1;
                    currentExpect = 4;
                }
                else if(chunk[i] == '\r')
                    currentExpect = 100;
                else if((chunk[i] != '0' && chunk[i] != '1') ||
                        i != currentBase)
                    throw runtime_error("malformed request");
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
                    throw runtime_error("malformed request");
                else if(CURRENT_ISUPPER)
                    headerKey[i - currentBase] = chunk[i] + 32;
                else if(CURRENT_ISLOWER || chunk[i] == '-' || chunk[i] == '_' ||
                        CURRENT_ISNUMBER)
                    headerKey[i - currentBase] = chunk[i];
                else
                    throw runtime_error("malformed request");
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
                    req->_headers[string(headerKey, verbOrKeyLength)]
                            = shared_ptr<string>(new string(CURRENT_VALUE));
                    currentBase = i + 1;
                    currentExpect = chunk[i] == '\r' ? 100 : 4;
                    break;
                }
                throw runtime_error("malformed request");
            case 100: // expect \n after '\r'
                if(chunk[i] != '\n') throw runtime_error("malformed request");
                currentBase = i + 1;
                currentExpect = 4;
                break;
            case 101: // expect final \n
                if(chunk[i] != '\n') throw runtime_error("malformed request");
                goto entire_request_decoded;
        }
        i++;
    }
    return false;
    entire_request_decoded:
    _msg = req;
    stb->pull(i + 1);
    return true;
}

shared_ptr<message> http_request::decoder::msg() {
    return _msg;
}

http_request::decoder::~decoder() {}