#include "xycommon.h"
#include <cstring>
#include <cstdarg>
#include <vector>
#include <execinfo.h>

std::string fmt(const char * const f, ...) {
    va_list ap, ap2;
    va_start(ap, f);
    va_copy(ap2, ap);
    const int len = vsnprintf(NULL, 0, f, ap2);
    va_end(ap2);
    vector<char> zc(len + 1);
    vsnprintf(zc.data(), zc.size(), f, ap);
    va_end(ap);
    return string(zc.data(), len); 
}

std::string timelabel() {
    time_t now = time(NULL);
    char tmlabel[32];
    int len = strftime(tmlabel, sizeof(tmlabel),
        "%Y-%m-%d %H:%M:%S", localtime(&now));
    return string(tmlabel, len);
}

extended_runtime_error::extended_runtime_error
    (const char *fname, int lno, const string &wh) :
    _filename(fname), _lineno(lno), runtime_error(wh) {
    _depth = backtrace(_btbuf, 20) - 1;
}

const char *extended_runtime_error::filename() {
    return strrchr(_filename, '/') ?
        strrchr(_filename, '/') + 1 : _filename;
}

int extended_runtime_error::lineno() {
    return _lineno;
}

int extended_runtime_error::tracedepth() {
    return _depth;
}

char **extended_runtime_error::stacktrace() {
    return backtrace_symbols(_btbuf + 1, _depth);
}