#ifndef XYHTTPD_HTTPSVC_H
#define XYHTTPD_HTTPSVC_H

#include "xyhttp.h"
#include <vector>

class http_service_chain : public http_service {
public:
    virtual void serve(shared_ptr<http_transaction> tx);
    virtual void append(shared_ptr<http_service> svc);
    inline shared_ptr<http_service> &operator[](int i) {
        return _svcs.at(i);
    }
    inline int size() const {
        return _svcs.size();
    }
private:
    vector<shared_ptr<http_service>> _svcs;
};

class local_file_service : public http_service {
public:
    local_file_service(const string &docroot);
    void register_mimetype(const string &ext, const string &type);
    virtual void serve(shared_ptr<http_transaction> tx);
private:
    string _docroot;
    vector<string> _defdocs;
    map<string, shared_ptr<string>> _mimetypes;
};

#endif