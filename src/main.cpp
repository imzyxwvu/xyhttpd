#include <iostream>
#include <uv.h>
#include "xyhttpsvc.h"

int main() {
    auto services = shared_ptr<http_service_chain>(new http_service_chain());
    http_server server(services);
    signal(SIGPIPE, SIG_IGN);
    try {
        auto svc_localfile = shared_ptr<local_file_service>
                (new local_file_service("htdocs"));
        svc_localfile->register_fcgi("php",
                shared_ptr<fcgi_provider>(new tcp_fcgi_provider("127.0.0.1", 9000)));
        services->append(make_shared<logger_service>(cout));
        services->append(svc_localfile);
        server.listen("0.0.0.0", 8080);
    }
    catch(exception &ex) {
        cerr<<"Can not start HTTP service: "<<ex.what()<<endl;
        return 1;
    }
    uv_run(uv_default_loop(), UV_RUN_DEFAULT);
    return 0;
}