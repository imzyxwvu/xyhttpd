#ifndef XYHTTPD_IOMASTER_H
#define XYHTTPD_IOMASTER_H

#include <xywebsocket.h>
#include <linux/nbd.h>
#include <cstring>
#include <map>

class nbd_server {
public:
    class io_request {
    public:
        io_request();
        ~io_request();
        inline int optype() { return _request.type; }
        inline int length() { return _request.len; }
        inline long long base() { return _request.from; }
        inline char *data() { return _buf; }
        char *complete(int err);
        friend class nbd_server;
    private:
        nbd_request _request;
        nbd_reply _reply;
        char *_buf;
    };

    class poll_scope {
    public:
        poll_scope(nbd_server *svr, int event);
        ~poll_scope();
    private:
        nbd_server *_svr;
        int _event;
    };

    nbd_server(int sock_fd, int nbd_fd);
    shared_ptr<io_request> poll_event();
    void finish_request(const shared_ptr<io_request> &req);
    virtual ~nbd_server();
    static shared_ptr<nbd_server> setup_local(const char *dev_file, size_t siz);

    shared_ptr<fiber> _pending_fiber, _flushing_fiber;
private:
    static void flush_requests(void *svr);
    void flush_requests();
    void update_poller(int events);

    int _fd_sock, _fd_nbd, _status;
    uv_poll_t *_poller;
    queue<shared_ptr<io_request>> _finished;
};

class block_provider {
public:
    virtual void read(long long blkno, char *outbuf) = 0;
    virtual void write(long long blkno, char *buf) = 0;
    virtual int block_count() = 0;
    inline int block_size() { return 4096; }
    virtual ~block_provider() = 0;
};

class file_block_provider : public block_provider {
public:
    class io_request {
    public:
        io_request(int type, int length);
        ~io_request();
    private:
        shared_ptr<fiber> _issuer;
        int _type;
        size_t _length;
        char *_buf;
    };

    file_block_provider(const char *filename);
    virtual void read(long long blkno, char *outbuf);
    virtual void write(long long blkno, char *buf);
    virtual int block_count();
    virtual ~file_block_provider();
private:
    struct stat _statbuf;
    int _fd;
};

#define IOLOG_BUFFER_SIZE 1048576

class io_mangler_provider : public block_provider {
public:
    struct iolog_entry {
        long long blk_no;
        clock_t tick;
        long long optype;
    };

    io_mangler_provider(const shared_ptr<block_provider> &baseProvider);
    virtual void read(long long blkno, char *outbuf);
    virtual void write(long long blkno, char *buf);
    virtual int block_count();
    void create_snapshot(int period, map<long long, int> &out, int &nread, int &nwrite);
    virtual ~io_mangler_provider();
private:
    void track_access(long long blkno, int type);
    iolog_entry _log_buffer[IOLOG_BUFFER_SIZE];
    int _latest_ptr;
    shared_ptr<block_provider> _base;
};

class io_request_dispatcher {
public:
    io_request_dispatcher(int scale, shared_ptr<block_provider> provider);
    io_request_dispatcher(const io_request_dispatcher &) = delete;
    void poll_events(shared_ptr<nbd_server> svr);
private:
    static void work_thread_wrap(void *self);
    void work_thread();

    queue<shared_ptr<nbd_server::io_request>> _work_queue;
    queue<shared_ptr<fiber>> _idle;
    shared_ptr<block_provider> _blk_provider;
    shared_ptr<nbd_server> _server;
};

#endif