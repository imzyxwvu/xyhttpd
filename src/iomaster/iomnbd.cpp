#include "iomaster.h"

#include <cstdlib>
#include <unistd.h>
#include <cassert>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <linux/nbd.h>
#include <cstdio>
#include <cerrno>
#include <csignal>

shared_ptr<nbd_server> nbd_server::setup_local(const char *dev_file, size_t siz)
{
    int sp[2];
    int nbd, sk, err;

    err = socketpair(AF_UNIX, SOCK_STREAM, 0, sp);
    assert(!err);

    nbd = open(dev_file, O_RDWR);
    if (nbd == -1) {
        fprintf(stderr,
                "Failed to open `%s': %s\n"
                "Is kernel module `nbd' loaded and you have permissions "
                "to access the device?\n", dev_file, strerror(errno));
        exit(EXIT_FAILURE);
    }

    ioctl(nbd, NBD_DISCONNECT);
    ioctl(nbd, NBD_CLEAR_QUE);
    assert(ioctl(nbd, NBD_SET_TIMEOUT, 600) != -1);
    assert(ioctl(nbd, NBD_SET_BLKSIZE, (u_int32_t)4096) != -1);
    assert(ioctl(nbd, NBD_SET_SIZE_BLOCKS, (u_int64_t)siz) != -1);
    assert(ioctl(nbd, NBD_CLEAR_SOCK) != -1);

    pid_t pid = fork();
    if (pid == 0) {
        /* Block all signals to not get interrupted in ioctl(NBD_DO_IT), as
         * it seems there is no good way to handle such interruption.*/
        sigset_t sigset;
        if (sigfillset(&sigset) != 0 || sigprocmask(SIG_SETMASK, &sigset, NULL) != 0) {
            perror("sigprocmask");
            exit(EXIT_FAILURE);
        }

        /* The child needs to continue setting things up. */
        close(sp[0]);
        sk = sp[1];

        if(ioctl(nbd, NBD_SET_SOCK, sk) == -1){
            fprintf(stderr, "ioctl(nbd, NBD_SET_SOCK, sk) failed.[%s]\n", strerror(errno));
            exit(EXIT_FAILURE);
        }

        if (ioctl(nbd, NBD_SET_FLAGS, 0) == -1){ // NBD_FLAG_SEND_FLUSH | NBD_FLAG_SEND_TRIM
            perror("ioctl(NBD_SET_FLAGS)");
            exit(EXIT_FAILURE);
        }

        err = ioctl(nbd, NBD_DO_IT);
        if (err == -1) {
            perror("ioctl(NBD_DO_IT)");
            exit(EXIT_FAILURE);
        }

        ioctl(nbd, NBD_CLEAR_QUE);
        ioctl(nbd, NBD_CLEAR_SOCK);

        exit(0);
    }

    close(sp[1]);
    return make_shared<nbd_server>(sp[0], nbd);
}

nbd_server::poll_scope::poll_scope(nbd_server *svr, int event)
        : _svr(svr), _event(event) {
    _svr->update_poller(_svr->_status | _event);
}

nbd_server::poll_scope::~poll_scope() {
    _svr->update_poller(_svr->_status & ~_event);
}

void nbd_server::flush_requests() {
    poll_scope scope(this, UV_WRITABLE);
    while(!_finished.empty()) {
        auto req = _finished.front();
        fiber::yield<int_status>();
        int nwrite = write(_fd_sock, &req->_reply, sizeof(req->_reply));
        if(nwrite < 0)
            throw runtime_error("failed to complete IO request");
        if(req->optype() == NBD_CMD_READ && req->_reply.error == 0 && req->_buf) {
            char *tx_base = req->_buf;
            size_t tx_rest = req->length();
            while(tx_rest) {
                fiber::yield<int_status>();
                nwrite = write(_fd_sock, tx_base, tx_rest);
                if(nwrite < 0) {
                    perror("write");
                    break;
                }
                tx_base += nwrite;
                tx_rest -= nwrite;
            }
        }
        _finished.pop();
    }
}

void nbd_server::flush_requests(void *svr) {
    nbd_server *self = (nbd_server *)svr;
    while(true) {
        self->flush_requests();
        fiber::yield();
    }
}

static void poll_callback(uv_poll_t* handle, int status, int events)
{
    auto svr = (nbd_server *)handle->data;
    if(events & UV_WRITABLE)
        svr->_flushing_fiber->resume(int_status::make(status));
    if(events & UV_READABLE)
        svr->_pending_fiber->resume(int_status::make(status));
}

void nbd_server::update_poller(int new_status) {
    if(new_status) {
        if(new_status != _status) {
            int r = uv_poll_start(_poller, new_status, poll_callback);
            if(r < 0)
                throw runtime_error("failed to update poller");
            _status = new_status;
        }
    } else {
        uv_poll_stop(_poller);
        _status = new_status;
    }
}

nbd_server::nbd_server(int sock_fd, int nbd_fd)
        : _fd_sock(sock_fd), _fd_nbd(nbd_fd), _status(0) {
    _poller = new uv_poll_t;
    if(uv_poll_init(uv_default_loop(), _poller, _fd_sock) < 0) {
        delete _poller;
        throw RTERR("failed to setup socket poller");
    }
    _poller->data = this;
    _flushing_fiber = fiber::make(nbd_server::flush_requests, this);
}

#ifdef WORDS_BIGENDIAN
u_int64_t ntohll(u_int64_t a) {
  return a;
}
#else
u_int64_t ntohll(u_int64_t a) {
    u_int32_t lo = a & 0xffffffff;
    u_int32_t hi = a >> 32U;
    lo = ntohl(lo);
    hi = ntohl(hi);
    return ((u_int64_t) lo) << 32U | hi;
}
#endif

shared_ptr<nbd_server::io_request> nbd_server::poll_event() {
    poll_scope scope(this, UV_READABLE);
    this->_pending_fiber = fiber::current();
    auto ioreq = make_shared<nbd_server::io_request>();
    while(true) {
        fiber::yield<int_status>();
        if(read(_fd_sock, &ioreq->_request, sizeof(ioreq->_request)) < 0) {
            if(errno == EAGAIN)
                continue;
            else
                throw runtime_error("disconnected");
        }
        break;
    }
    ioreq->_request.type = ntohl(ioreq->_request.type);
    ioreq->_request.len = ntohl(ioreq->_request.len);
    ioreq->_request.from = ntohll(ioreq->_request.from);
    if(ioreq->optype() == NBD_CMD_WRITE) {
        ioreq->_buf = (char*)malloc(ioreq->length());
        char *base = ioreq->_buf;
        size_t rest = ioreq->length();
        while(rest) {
            fiber::yield<int_status>();
            int nread = read(_fd_sock, base, rest);
            if(nread < 0) {
                perror("read");
                break;
            }
            base += nread;
            rest -= nread;
        }
    }
    this->_pending_fiber.reset();
    return ioreq;
}

void nbd_server::finish_request(const shared_ptr<io_request> &req) {
    bool shouldResumeFlushThread = _finished.empty();
    _finished.push(req);
    if(shouldResumeFlushThread)
        _flushing_fiber->resume();
}

nbd_server::~nbd_server() {
    uv_close((uv_handle_t *)_poller, (uv_close_cb)free);
    close(_fd_sock);
}

nbd_server::io_request::io_request() : _buf(NULL) {}

char* nbd_server::io_request::complete(int err) {
    _reply.magic = htonl(NBD_REPLY_MAGIC);
    memcpy(_reply.handle, _request.handle, sizeof(_reply.handle));
    _reply.error = htonl(err);
    if(optype() == NBD_CMD_READ && err == 0) {
        _buf = (char *)malloc(length());
        return _buf;
    }
    return nullptr;
}

nbd_server::io_request::~io_request() {
    if(_buf) free(_buf);
}

void io_request_dispatcher::work_thread_wrap(void *void_self) {
    auto self = (io_request_dispatcher *)void_self;
    while(self->_blk_provider) {
        self->work_thread();
        self->_idle.push(fiber::current());
        fiber::yield();
    }
}

io_request_dispatcher::io_request_dispatcher(int scale, shared_ptr<block_provider> provider)
        : _blk_provider(provider) {
    for(int i = 0; i < scale; i++)
        _idle.push(fiber::make(io_request_dispatcher::work_thread_wrap, this));
}

void io_request_dispatcher::work_thread() {
    while(!_work_queue.empty()) {
        auto job = _work_queue.front();
        int block = job->base() / _blk_provider->block_size();
        int restLen = job->length();
        _work_queue.pop();
        switch(job->optype()) {
            case NBD_CMD_READ: {
                if(job->base() % _blk_provider->block_size() > 0) {
                    job->complete(EINVAL);
                    _server->finish_request(job);
                    continue;
                }
                char *buf = (char *)malloc(_blk_provider->block_size());
                char *iorbuf = job->complete(0);
                while(restLen) {
                    int copyLength = _blk_provider->block_size();
                    if(restLen < copyLength) copyLength = restLen;
                    _blk_provider->read(block++, buf);
                    memcpy(iorbuf, buf, copyLength);
                    iorbuf += copyLength;
                    restLen -= copyLength;
                }
                free(buf);
                _server->finish_request(job);
                break;
            }
            case NBD_CMD_WRITE:{
                char *iorbuf = job->data();
                if(job->base() % _blk_provider->block_size() > 0) {
                    job->complete(EINVAL);
                    _server->finish_request(job);
                    continue;
                }
                while(restLen) {
                    if(restLen < _blk_provider->block_size()) {
                        char *modbuf = (char *)malloc(_blk_provider->block_size());
                        _blk_provider->read(block, modbuf);
                        memcpy(modbuf, iorbuf, restLen);
                        _blk_provider->write(block++, modbuf);
                        delete modbuf;
                        iorbuf += restLen;
                        restLen -= restLen;
                    } else {
                        _blk_provider->write(block++, iorbuf);
                        iorbuf += _blk_provider->block_size();
                        restLen -= _blk_provider->block_size();
                    }
                }
                job->complete(0);
                _server->finish_request(job);
                break;
            }
        }
    }
}

void io_request_dispatcher::poll_events(shared_ptr<nbd_server> svr) {
    _server = move(svr);
    while(auto req = _server->poll_event()) {
        _work_queue.push(req);
        if(!_idle.empty()) {
            auto worker = _idle.front();
            _idle.pop();
            worker->resume();
        }
    }
}