#include <iostream>
#include <uv.h>
#include "xyhttp.h"

int main() {
    http_server server(new http_service());
    signal(SIGPIPE, SIG_IGN);
    try {
        server.listen("0.0.0.0", 8080);
    }
    catch(exception &ex) {
        cerr<<"Can not start HTTP service: "<<ex.what()<<endl;
        return 1;
    }
    uv_run(uv_default_loop(), UV_RUN_DEFAULT);
    return 0;
}