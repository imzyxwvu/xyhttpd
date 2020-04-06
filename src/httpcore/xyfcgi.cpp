#include <cstring>
#include <sstream>
#include <iostream>
#include <utility>
#include "xyfcgi.h"

using namespace std;

bool fcgi_message::decoder::decode(stream_buffer &stb) {
    if(stb.size() >= 8) {
        unsigned char *buf = (unsigned char *)stb.data();
        unsigned char msgType = buf[1];
        unsigned short requestId = (buf[2] << 8) | buf[3];
        unsigned short length = (buf[4] << 8) | buf[5];
        unsigned char paddingLen = buf[6];
        if(buf[0] != 1)
            throw runtime_error("FastCGI version error");
        size_t expectedLength = 8 + length + paddingLen;
        if(stb.size() >= expectedLength) {
            _msg = make_shared<fcgi_message>(
                    (message_type)msgType, requestId, (char *)buf + 8, length);
            stb.pull(expectedLength);
            return true;
        }
    }
    return false;
}

fcgi_message::decoder::~decoder() = default;

fcgi_message::fcgi_message(fcgi_message::message_type t, int requestId)
        : _type(t), _request_id(requestId) {}

fcgi_message::fcgi_message(fcgi_message::message_type t, int requestId, const char *data, int len)
    : _type(t), _request_id(requestId), _payload(data, len) {}

fcgi_message::~fcgi_message() = default;

void fcgi_message::serialize(char *buf) {
    auto *hdr = (unsigned char *)buf;
    hdr[0] = 1; // FCGI_VERSION_1
    hdr[1] = _type;
    hdr[2] = (_request_id >> 8) & 0xff;
    hdr[3] = _request_id & 0xff;
    hdr[4] = (_payload.size() >> 8) & 0xff;
    hdr[5] = _payload.size() & 0xff;
    hdr[6] = 0;
    hdr[7] = 0;
    if(_payload)
        memcpy(buf + 8, _payload.data(), _payload.size());
}

int fcgi_message::serialize_size() {
    return 8 + _payload.size();
}

chunk fcgi_connection::get_env(const std::string &key) {
    return _env[key];
}

shared_ptr<fcgi_message> fcgi_message::make_dummy(fcgi_message::message_type t) {
    return make_shared<fcgi_message>(t, 0);
}

fcgi_connection::fcgi_connection(P<stream> strm, int roleId)
	: _strm(move(strm)), _env_sent(false), _decoder(make_shared<fcgi_message::decoder>()) {
    unsigned char requestBegin[8] = { 0, (unsigned char)roleId, 0, 0, 0, 0, 0, 0};
    _strm->write(make_shared<fcgi_message>(
            fcgi_message::message_type::FCGI_BEGIN_REQUEST, 0, (char *)requestBegin, 8));
}

void fcgi_connection::set_env(const string &key, chunk val) {
    if(_env_sent)
        throw RTERR("environment variables already sent");
    if(!val) return;
    _env[key] = move(val);
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
    if(_env_sent) return;
    stringstream ss;
    for(auto it = _env.cbegin(); it != _env.cend(); it++) {
        if(!it->second) continue;
        append_length_bytes(ss, it->first.size());
        append_length_bytes(ss, it->second.size());
        ss.write(it->first.data(), it->first.size());
        ss.write(it->second.data(), it->second.size());
    }
    _strm->write(make_shared<fcgi_message>(
            fcgi_message::message_type::FCGI_PARAMS, 0, ss.str().data(), ss.str().size()));
    _strm->write(fcgi_message::make_dummy(fcgi_message::message_type::FCGI_PARAMS));
    _env_sent = true;
}

void fcgi_connection::write(const char *data, int len) {
    flush_env();
    _strm->write(make_shared<fcgi_message>(fcgi_message::message_type::FCGI_STDIN, 0, data, len));
}

void fcgi_connection::write(const chunk &msg) {
    write(msg.data(), msg.size());
}

chunk fcgi_connection::read() {
    flush_env();
    while(true) {
        auto msg = _strm->read<fcgi_message>(_decoder);
        switch(msg->msgtype()) {
            case fcgi_message::message_type::FCGI_STDOUT:
                return msg->data();
            case fcgi_message::message_type::FCGI_STDERR:
                cerr.write(msg->data().data(), msg->data().size());
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
    : _path(p) {}

shared_ptr<fcgi_connection> unix_fcgi_provider::get_connection()
{
    auto strm = make_shared<unix_stream>();
    strm->connect(_path);
    return make_shared<fcgi_connection>(strm, 1);
}
