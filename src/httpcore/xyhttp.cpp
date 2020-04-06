#include <iostream>
#include <cstring>

#include "xyhttp.h"

using namespace std;

http_connection::http_connection(P<stream> strm, string pname)
    : _reqdec(make_shared<http_request::decoder>()), _strm(strm),
      _keep_alive(true), _upgraded(false), _peername(pname) {}

P<http_request> http_connection::next_request() {
    try {
        P<http_request> _req = _strm->read<http_request>(_reqdec);
        chunk connhdr = _req->header("connection");
        if(connhdr) {
            _keep_alive = connhdr.find("keep-alive") != -1 ||
                          connhdr.find("Keep-Alive") != -1;
        } else {
            _keep_alive = false;
        }
        return _req;
    }
    catch(exception &ex) {
        _keep_alive = false;
        _strm.reset();
        return nullptr;
    }
}

bool http_connection::keep_alive() {
    return _keep_alive && _strm && !_upgraded;
}

shared_ptr<stream> http_connection::upgrade() {
    _upgraded = true;
    _strm->set_timeout(0);
    return _strm;
}

void http_connection::invoke_service(const P<http_service> &svc, http_trx &tx) {
    try {
        if(!tx->request->header("host")) {
            tx->display_error(400);
            _strm.reset();
            return;
        }
        svc->serve(tx);
        if(!tx->header_sent())
            tx->display_error(404);
    }
    catch(extended_runtime_error &ex) {
        if(tx->header_sent()) {
            cerr<<"["<<timelabel()<<" "<<peername()<<"] ";
            cerr<<ex.filename()<<":"<<ex.lineno()<<": "<<ex.what()<<endl;
            _strm.reset();
            return;
        }
        auto resp = tx->get_response(500);
        resp->set_header("Content-Type", "text/html");
        tx->write(fmt("<html><head><title>XWSG Internal Error</title></head>"
                      "<body><h1>500 Internal Server Error</h1><p><span>%s:%d:</span> %s</p>"
                      "<ul style=\"color:gray\">", ex.filename(), ex.lineno(), ex.what()));
        char **trace = ex.stacktrace();
        for(int i = 0; i < ex.tracedepth(); i++)
            tx->write(fmt("<li>%s</li>", trace[i]));
        free(trace);
        tx->write(fmt("</ul><i style=\"font-size:.8em\">%s</i></body></html>",
                      http_transaction::SERVER_VERSION.c_str()));
    }
    catch(exception &ex) {
        if(tx->header_sent()) {
            cerr<<"["<<timelabel()<<" "<<peername()<<"] "<<ex.what()<<endl;
            _strm.reset();
            return;
        }
        auto resp = tx->get_response(500);
        resp->set_header("Content-Type", "text/html");
        tx->write(fmt("<html>"
                      "<head><title>XWSG Internal Error</title></head>"
                      "<body><h1>500 Internal Server Error</h1>"
                      "<p>%s</p></body></html>", ex.what()));
    }
    tx->finish();
}

bool http_connection::has_tls() {
    return _strm->has_tls();
}

http_server::http_server(shared_ptr<http_service> svc) : service(svc) {
    _server = mem_alloc<uv_tcp_t>();
    if(uv_tcp_init(uv_default_loop(), _server) < 0) {
        free(_server);
        throw runtime_error("failed to initialize libuv TCP stream");
    }
    _server->data = this;
}

http_server::http_server(http_service *svc)
        : http_server(shared_ptr<http_service>(svc)) {}

static void http_server_on_connection(uv_stream_t* strm, int status) {
    http_server *self = (http_server *)strm->data;
    if(status >= 0) {
        P<tcp_stream> client = make_shared<tcp_stream>();
        try {
            client->accept(strm);
            client->nodelay(true);
            /*
             * Some sockets may have received RST frames before we accept them.
             * getpeername() on these sockets leads to ENOTCONN exceptions, which
             * may help filtering reset sockets.
             */
            P<ip_endpoint> peer = client->getpeername();
            fiber::launch(bind(&http_server::service_loop, self,
                               make_shared<http_connection>(client, peer->straddr())));
        }
        catch(exception &ex) {
            return;
        }
    }
}

void http_server::service_loop(P<http_connection> conn) {
    while(conn->keep_alive()) {
        shared_ptr<http_request> request = conn->next_request();
        if(!request) break;
        try {
            conn->invoke_service(service,
                                 make_shared<http_transaction>(conn, move(request)));
        }
        catch(exception &ex) {
            cerr<<"["<<timelabel()<<" "<<conn->peername()<<"] "<<ex.what()<<endl;
            break;
        }
    }
}

void http_server::listen(const char *addr, int port) {
    sockaddr_storage saddr;
    if(uv_ip4_addr(addr, port, (struct sockaddr_in*)&saddr) &&
       uv_ip6_addr(addr, port, (struct sockaddr_in6*)&saddr))
        throw invalid_argument("invalid IP address or port");
    int r = uv_tcp_bind(_server, (struct sockaddr *)&saddr, 0);
    if(r < 0)
        throw runtime_error(uv_strerror(r));
    do_listen(64);
}

void http_server::do_listen(int backlog) {
    int r = uv_listen((uv_stream_t *)_server, backlog, http_server_on_connection);
    if(r < 0)
        throw runtime_error(uv_strerror(r));
}

http_server::~http_server() {
    if(_server) {
        uv_close((uv_handle_t *)_server, (uv_close_cb) free);
    }
}

/****** HTTP message decoding ******/

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
        _path = url_decode(_resource.data(), queryBase - _resource.data());
        _query = chunk(queryBase + 1);
    } else {
        _path = url_decode(_resource.data(), _resource.size());
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

http_request::decoder::~decoder() {}

std::string http_request::url_decode(const char *buf, int siz) {
    string result;
    char hexstr[3];
    hexstr[2] = 0;
    result.reserve(siz);
    while(--siz >= 0) {
        unsigned char in = *buf;
        if ('%' == in && siz >= 2 &&
            isxdigit(buf[1]) && isxdigit(buf[2])) {
            // this is two hexadecimal digits following a '%'
            hexstr[0] = buf[1], hexstr[1] = buf[2];
            unsigned int hex = strtoul(hexstr, nullptr, 16);
            in = (unsigned char) hex; // this long is never bigger than 255 anyway
            buf += 2;
            siz -= 2;
        }
        else if('+' == in)
            in = ' ';
        result.push_back(in);
        buf++;
    }
    return result;
}

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
            _msg = make_shared<string_message>(end + 2, _nBytes);
            stb.pull(chunkSize);
            return true;
        }
        return false;
    } else {
        return string_decoder::decode(stb);
    }
}

http_client::http_client(P<stream> strm) : _stream(move(strm)),
                                           _resp_decoder(make_shared<http_response::decoder>()), _reusable(true) {}

P<http_response> http_client::send(const P<http_request> &request) {
    if(data_available())
        throw RTERR("pending response for last request");
    if(!_reusable)
        throw RTERR("connection is over");
    try {
        _stream->write(request);
        auto response = _stream->read<http_response>(_resp_decoder);
        auto connection = response->header("Connection");
        if(!connection || (
                connection.find("keep-alive") == -1 &&
                connection.find("Keep-Alive") == -1))
            _reusable = false;
        _tsfr_decoder = make_shared<http_transfer_decoder>(response);
        return response;
    }
    catch(runtime_error &ex) {
        _reusable = false;
        throw;
    }
}

chunk http_client::read() {
    if(!data_available())
        throw RTERR("read on unreadable HTTP client connection");

    auto msg = _stream->read<string_message>(_tsfr_decoder);
    if(!_tsfr_decoder->more()) // Response is over
        _tsfr_decoder.reset();
    return msg->str();
}

http_client::~http_client() = default;