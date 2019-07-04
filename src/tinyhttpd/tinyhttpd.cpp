#include "xyhttpsvc.h"
#include "xyhttptls.h"
#include <getopt.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <iostream>

string pwd() {
    char pathbuf[PATH_MAX];
    if(!getcwd(pathbuf, sizeof(pathbuf))) {
        perror("getcwd");
        exit(EXIT_FAILURE);
    }
    return string(pathbuf);
}

int main(int argc, char *argv[])
{
    char bindAddr[NAME_MAX] = "0.0.0.0";
    int port = 8080;
    shared_ptr<local_file_service> local_file_svc(
            new local_file_service(pwd()));
    char *portBase;
    char suffix[16];
    char fcgiHandler[NAME_MAX];
    int opt;
    shared_ptr<fcgi_provider> fcgiProvider;
    while ((opt = getopt(argc, argv, "r:b:f:d:h")) != -1) {
        switch(opt) {
            case 'r':
                local_file_svc->set_document_root(optarg);
                break;
            case 'b':
                strcpy(bindAddr, optarg);
                portBase = strchr(bindAddr, ':');
                if(portBase) {
                    *portBase = 0;
                    port = atoi(portBase + 1);
                    if(port == 0) {
                        printf("Invalid port number - %d.\n", port);
                        return EXIT_FAILURE;
                    }
                }
                break;
            case 'f':
                portBase = strchr(optarg, '=');
                if(!portBase) {
                    printf("Invalid FastCGI handler - %s.\n", optarg);
                    return EXIT_FAILURE;
                }
                *portBase = 0;
                strcpy(suffix, optarg);
                strcpy(fcgiHandler, portBase + 1);
                portBase = strchr(fcgiHandler, ':');
                if(portBase) {
                    *portBase = 0;
                    fcgiProvider = make_shared<tcp_fcgi_provider>(
                            fcgiHandler, atoi(portBase + 1));
                } else {
                    fcgiProvider = make_shared<unix_fcgi_provider>(
                            fcgiHandler);
                }
                local_file_svc->register_fcgi(suffix, fcgiProvider);
                break;
            case 'd':
                local_file_svc->add_defdoc_name(optarg);
                break;
            default:
                printf("Invalid argument - %c.\n\n", opt);
            case 'h':
                printf("Usage: %s [-h] [-r htdocs] [-b 0.0.0.0:8080] "
                       "[-f FcgiProvider] [-d index.php]\n\n", argv[0]);
                puts("   -h\tShow help information");
                puts("   -r\tSet document root");
                puts("   -b\tSet bind address and port");
                puts("   -f\tAdd FastCGI suffix and handler");
                puts("   -d\tAdd default document search name");
                puts("");
                return EXIT_FAILURE;
        }
    }
    signal(SIGPIPE, SIG_IGN);
    shared_ptr<http_service_chain> svcChain(new http_service_chain());
    http_server server(svcChain);
    try {
        svcChain->append(make_shared<logger_service>(cout));
        svcChain->append(local_file_svc);
        server.listen(bindAddr, port);
    }
    catch(runtime_error &ex) {
        printf("Failed to bind port %d: %s\n", port, ex.what());
    }
    uv_run(uv_default_loop(), UV_RUN_DEFAULT);
    return EXIT_SUCCESS;
}