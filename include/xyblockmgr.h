#ifndef XYHTTPD_XYBLOCKMGR_H
#define XYHTTPD_XYBLOCKMGR_H

#include "xycommon.h"

class block_stream {
public:
    inline int size() { return _size; }
    inline char *data() { return _data; }
    inline int blkid() { return _blkid; }
    virtual bool next();
    virtual void commit(bool creat);
    virtual void reset();
    virtual void truncate();
    block_stream(const block_stream &);
    virtual ~block_stream();
private:
    block_stream(shared_ptr<class block_manager> _mgr, int _bufsiz);
    char *_data;
    int _size, _blkid, _entry;
    bool _locked;
    shared_ptr<class block_manager> _blkmgr;
};

class block_manager {
public:
    virtual int block_count() = 0;
    virtual shared_ptr<block_stream> root_stream();
    virtual shared_ptr<block_stream> create_stream();
    virtual shared_ptr<block_stream> get_stream(int blkId);
    virtual bool fill_next(shared_ptr<block_stream> strm);
    virtual void commit(shared_ptr<block_stream> strm, bool creat);
    virtual void truncate(shared_ptr<block_stream> strm);
    virtual ~block_manager() = 0;
private:
    virtual bool fill(shared_ptr<block_stream> strm) = 0;
    virtual bool persist(shared_ptr<block_stream> strm) = 0;
    friend class block_stream;
};

class memory_block_manager : public block_manager {

};

#endif
