#include "iomaster.h"

#include <xyhttpsvc.h>
#include <unistd.h>
#include <sys/time.h>
#include <sstream>
#include <iostream>

block_provider::~block_provider() {

}

file_block_provider::file_block_provider(const char *filename) {
    if(stat(filename, &_statbuf) < 0)
        throw RTERR("failed to stat backend file: %s", strerror(errno));
    _fd = open(filename, O_RDWR);
    if(_fd < 0)
        throw RTERR("failed to open backend file: %s", strerror(errno));
}

void file_block_provider::read(long long blkno, char *outbuf) {
    lseek(_fd, blkno * block_size(), SEEK_SET);
    ::read(_fd, outbuf, block_size());
}

void file_block_provider::write(long long blkno, char *buf) {
    lseek(_fd, blkno * block_size(), SEEK_SET);
    ::write(_fd, buf, block_size());
}

int file_block_provider::block_count() {
    return _statbuf.st_size / block_size();
}

file_block_provider::~file_block_provider() {
    close(_fd);
}

inline long long time_ms() {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec * 1000 + (tv.tv_usec / 1000000);
}

io_mangler_provider::io_mangler_provider(const shared_ptr<block_provider> &baseProvider)
: _base(baseProvider), _latest_ptr(0), _log_buffer() {
}

int io_mangler_provider::block_count() {
    return _base->block_count();
}

void io_mangler_provider::read(long long blkno, char *outbuf) {
    track_access(blkno / 256, 0);
    _base->read(blkno, outbuf);
}

void io_mangler_provider::write(long long blkno, char *outbuf) {
    track_access(blkno / 256, 1);
    _base->read(blkno, outbuf);
}

void io_mangler_provider::track_access(long long blkno, int type) {
    _latest_ptr = (_latest_ptr + 1) % IOLOG_BUFFER_SIZE;
    _log_buffer[_latest_ptr].optype = type;
    _log_buffer[_latest_ptr].blk_no = blkno;
    _log_buffer[_latest_ptr].tick = time_ms();
}

void io_mangler_provider::create_snapshot(int period, map<long long, int> &out, int &nread, int &nwrite) {
    long long now = time_ms();
    for(int i = 0; i < IOLOG_BUFFER_SIZE; i++) {
        if(now - _log_buffer[i].tick < period) {
            switch(_log_buffer[i].optype) {
                case 0:
                    nread++;
                    break;
                case 1:
                    nwrite++;
                    break;
            }
            out[_log_buffer[i].blk_no]++;
        }
    }
}

io_mangler_provider::~io_mangler_provider() {}

shared_ptr<io_mangler_provider> provider;

static void serve_device_request(void *svr_ptr)
{
  auto server = *(shared_ptr<nbd_server> *)svr_ptr;
  io_request_dispatcher dispatcher(20, provider);
  dispatcher.poll_events(server);
}

message_broadcaster listeners;

void create_snapshot(uv_timer_t* handle)
{
    map<long long, int> iolog;
    int nread = 0, nwrite = 0;
    provider->create_snapshot(500, iolog, nread, nwrite);
    stringstream ss;
    ss << fmt("{\"read\":%.2f,\"write\":%.2f,\"heatmap\":{",  (double)nread / 128, (double)nwrite / 128);
    for(auto it = iolog.begin(); it != iolog.end(); it++) {
        if(it != iolog.begin()) ss<<',';
        ss<<fmt("\"%lld\":%d", it->first, it->second);
    }
    ss<<"}}";
    listeners.send(ss.str());
}

class iomaster_service : public http_service {
    virtual void serve(shared_ptr<http_transaction> tx);
};

void iomaster_service::serve(shared_ptr<http_transaction> tx) {
    shared_ptr<websocket> ws = tx->accept_websocket();
    ws->send(fmt("{\"block_count\":%d}", provider->block_count() / 256));
    listeners.add_sink(ws);
}

int main(int argc, char *argv[])
{
    auto base_provider = make_shared<file_block_provider>("test.dat");
    provider = make_shared<io_mangler_provider>(base_provider);
    auto server = nbd_server::setup_local("/dev/nbd13", provider->block_count());
    auto svcfiber = fiber::make(serve_device_request, &server);

    auto file_svc = make_shared<local_file_service>("htdocs/iomaster");
    file_svc->add_defdoc_name("index.html");
    auto web_svc = make_shared<http_service_chain>();
    web_svc->append<logger_service>(cout);
    web_svc->route<iomaster_service>("/connect");
    web_svc->append(file_svc);
    http_server svr(web_svc);
    svr.listen("0.0.0.0", 8033);

    signal(SIGPIPE, SIG_IGN);
    svcfiber->resume();
    uv_timer_t *timer = new uv_timer_t;
    uv_timer_init(uv_default_loop(), timer);
    uv_timer_start(timer, create_snapshot, 500, 500);
    uv_run(uv_default_loop(), UV_RUN_DEFAULT);
    return 0;
}
