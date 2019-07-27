#include <cstring>
#include <sstream>
#include <iostream>
#include "xyfcgi.h"

bool fcgi_message::decoder::decode(shared_ptr <streambuffer> &stb) {
    if(stb->size() >= 8) {
        unsigned char *buf = (unsigned char *)stb->data();
        unsigned char msgType = buf[1];
        unsigned short requestId = (buf[2] << 8) | buf[3];
        unsigned short length = (buf[4] << 8) | buf[5];
        unsigned char paddingLen = buf[6];
        if(buf[0] != 1)
            throw runtime_error("FastCGI version error");
        size_t expectedLength = 8 + length + paddingLen;
        if(stb->size() >= expectedLength) {
            _msg = make_shared<fcgi_message>(
                    (message_type)msgType, requestId, (char *)buf + 8, length);
            stb->pull(expectedLength);
            return true;
        }
    }
    return false;
}

shared_ptr<message> fcgi_message::decoder::msg() {
    return _msg;
}

fcgi_message::decoder::~decoder() {
}

fcgi_message::fcgi_message(fcgi_message::message_type t, int requestId)
        : _type(t), _request_id(requestId), _payload(nullptr), _payload_length(0) {}

fcgi_message::fcgi_message(fcgi_message::message_type t, int requestId, const char *data, int len)
    : _type(t), _request_id(requestId), _payload(nullptr), _payload_length(len) {
    if(len > 0) {
        if(!data)
            throw RTERR("payload length was set but payload is NULL");
        _payload = new char[len];
        memcpy(_payload, data, _payload_length);
    }
}

fcgi_message::~fcgi_message() {
    if(_payload) {
        delete[] _payload;
    }
}

void fcgi_message::serialize(char *buf) {
    unsigned char *hdr = (unsigned char *)buf;
    hdr[0] = 1; // FCGI_VERSION_1
    hdr[1] = _type;
    hdr[2] = (_request_id >> 8) & 0xff;
    hdr[3] = _request_id & 0xff;
    hdr[4] = (_payload_length >> 8) & 0xff;
    hdr[5] = _payload_length & 0xff;
    hdr[6] = 0;
    hdr[7] = 0;
    if(_payload)
        memcpy(buf + 8, _payload, _payload_length);
}

int fcgi_message::serialize_size() {
    return 8 + length();
}

shared_ptr<fcgi_message> fcgi_message::make_dummy(fcgi_message::message_type t) {
    return make_shared<fcgi_message>(t, 0);
}

fcgi_connection::fcgi_connection(const shared_ptr<stream> &strm, int roleId)
	: _strm(strm), _envready(false), _buffer(make_shared<streambuffer>()) {
    unsigned char requestBegin[8] = { 0, (unsigned char)roleId, 0, 0, 0, 0, 0, 0};
    _strm->write(make_shared<fcgi_message>(
            fcgi_message::message_type::FCGI_BEGIN_REQUEST, 0, (char *)requestBegin, 8));
}

void fcgi_connection::set_env(const string &key, const string &val) {
    set_env(key, make_shared<string>(val));
}

void fcgi_connection::set_env(const string &key, shared_ptr<string> val) {
    if(_envready)
        throw RTERR("environment variables already sent");
    if(!val) return;
    _env[key] = val;
}

static void append_length_bytes(stringstream &ss, int len) {
    if(len <= 127)
        ss.put(len);
    else {
        char buf[4];
        buf[0] = 0x80 | ((len >> 24) & 0x7f);
        buf[1] = (len >> 16) & 0xff;
        buf[2] = (len >> 8) & 0xff;
        buf[3] = len & 0xff;
        ss.write(buf, 4);
    }
}

void fcgi_connection::flush_env() {
    if(_envready) return;
    stringstream ss;
    for(auto it = _env.cbegin(); it != _env.cend(); it++) {
        if(!it->second) continue;
        append_length_bytes(ss, it->first.size());
        append_length_bytes(ss, it->second->size());
        ss.write(it->first.data(), it->first.size());
        ss.write(it->second->data(), it->second->size());
    }
    _strm->write(make_shared<fcgi_message>(
            fcgi_message::message_type::FCGI_PARAMS, 0, ss.str().data(), ss.str().size()));
    _strm->write(fcgi_message::make_dummy(fcgi_message::message_type::FCGI_PARAMS));
    _envready = true;
}

void fcgi_connection::write(const char *data, int len) {
    flush_env();
    _strm->write(make_shared<fcgi_message>(fcgi_message::message_type::FCGI_STDIN, 0, data, len));
}

void fcgi_connection::write(shared_ptr<string> msg) {
    write(msg->data(), msg->size());
}

shared_ptr<message> fcgi_connection::read(shared_ptr<decoder> decoder) {
    flush_env();
    if(decoder->decode(_buffer))
        return decoder->msg();
    while(true) {
        auto msg = _strm->read<fcgi_message>(make_shared<fcgi_message::decoder>());
        switch(msg->msgtype()) {
            case fcgi_message::message_type::FCGI_STDOUT:
                _buffer->append(msg->data(), msg->length());
                try {
                    if(decoder->decode(_buffer))
                        return decoder->msg();
                }
                catch(exception &ex) {
                    throw RTERR("Protocol Error: %s", ex.what());
                }
                break;
            case fcgi_message::message_type::FCGI_STDERR:
                cerr.write(msg->data(), msg->length());
                break;
            case fcgi_message::message_type::FCGI_END_REQUEST:
                return nullptr;
            default:
                throw RTERR("FastCGI protocol error");
        }
    }
}

tcp_fcgi_provider::tcp_fcgi_provider(const string &host, int port)
	: _hostip(host), _port(port) {}

shared_ptr<fcgi_connection> tcp_fcgi_provider::get_connection()
{
    auto strm = make_shared<tcp_stream>();
    strm->connect(_hostip, _port);
    return make_shared<fcgi_connection>(strm, 1);
}

unix_fcgi_provider::unix_fcgi_provider(const string &p)
    : _path(make_shared<string>(p)) {}

unix_fcgi_provider::unix_fcgi_provider(shared_ptr<string> p)
    : _path(p) {}

shared_ptr<fcgi_connection> unix_fcgi_provider::get_connection()
{
    auto strm = make_shared<unix_stream>();
    strm->connect(_path);
    return make_shared<fcgi_connection>(strm, 1);
}
