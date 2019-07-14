#include "xyhttpsvc.h"
#include "xyhttptls.h"
#include <getopt.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <iostream>
#include <fstream>

class signal_watcher {
public:
    signal_watcher(int signo) {
        int r = uv_signal_init(uv_default_loop(), &sig);
        if(r < 0) throw IOERR(r);
        uv_signal_start(&sig, signal_cb, signo);
    };
    signal_watcher(const signal_watcher &sig) = delete;
    ~signal_watcher() {
        uv_close((uv_handle_t *)&sig, nullptr);
    }
private:
    static void signal_cb(uv_signal_t* handle, int signum) {
        switch(signum) {
            case SIGINT:
            case SIGTERM:
                uv_stop(uv_default_loop());
                break;
        }
    }
    uv_signal_t sig;
};

string pwd() {
    char pathbuf[PATH_MAX];
    if(!getcwd(pathbuf, sizeof(pathbuf))) {
        perror("getcwd");
        exit(EXIT_FAILURE);
    }
    return string(pathbuf);
}

void print_usage(const char *progname) {
    printf("\n"
           "Usage: %s [-h] [-r htdocs] [-b 0.0.0.0:8080] [-d index.php]\n"
           "       [-f FcgiProvider] [-t sfx=MIME] [-p 127.0.0.1:90]\n\n", progname);
    puts("   -h\tShow this help information");
    puts("   -r\tSet path to document root directory. If not set, current working ");
    puts("     \tdirectory is used for convenience file sharing.");
    puts("   -b\tSet bind address and port");
    puts("   -s\tEnable TLS and use provided X509 certificate chain : PEM key pair");
    puts("   -d\tAdd default document search name.");
    puts("   -f\tAdd dynamic page suffix and its FastCGI handler.");
    puts("     \tTCP IP:port pair or UNIX domain socket path is accepted.");
    puts("   -t\tAdd MIME Type for suffix.");
    puts("   -p\tAdd proxy pass backend service. If multiple services are defined,");
    puts("     \tthey will be used in a round-robin machanism for load balancing.");
    puts("   -l\tSpecify HTTP access log file name.");
    puts("   -D\tBecome a background daemon process.");
    puts("");
}

void become_daemon()
{
    pid_t child = fork();
    if(child < 0) {
        perror("fork");
        exit(EXIT_FAILURE);
    }
    else if(child > 0) {
        printf("tinyhttpd started in background, PID = %d\n", child);
        exit(EXIT_SUCCESS);
    }
    setsid(); // Detach controlling terminal
    close(0); // Close stdin stream
    open("/dev/null", O_RDONLY);
}

int main(int argc, char *argv[])
{
    char bindAddr[NAME_MAX] = "0.0.0.0";
    int port = 8080;
    shared_ptr<local_file_service> local_file_svc(
            new local_file_service(pwd()));
    shared_ptr<proxy_pass_service> proxy_svc(new proxy_pass_service);
    char *portBase;
    char suffix[16];
    char backend[NAME_MAX];
    int opt;
    tls_context ctx;
    bool useTLS = false, daemonize = false;
    shared_ptr<fcgi_provider> fcgiProvider;
    shared_ptr<http_server> server;
    local_file_svc->register_mimetype("html", "text/html");
    local_file_svc->register_mimetype("css", "text/css");
    ostream *logStream = nullptr;
    while ((opt = getopt(argc, argv, "r:b:f:d:p:t:s:l:Dh")) != -1) {
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
            case 's':
                if(server) {
                    printf("Duplicate -s.\n");
                    return EXIT_FAILURE;
                }
                portBase = strchr(optarg, ':');
                if(portBase) {
                    *portBase = 0;
                    ctx.use_certificate(optarg, portBase + 1);
                } else {
                    ctx.use_certificate(optarg);
                }
                useTLS = true;
                break;
            case 'f':
                portBase = strchr(optarg, '=');
                if(!portBase) {
                    printf("Invalid FastCGI handler - %s.\n", optarg);
                    return EXIT_FAILURE;
                }
                *portBase = 0;
                strcpy(suffix, optarg);
                strcpy(backend, portBase + 1);
                portBase = strchr(backend, ':');
                if(portBase) {
                    *portBase = 0;
                    fcgiProvider = make_shared<tcp_fcgi_provider>(
                            backend, atoi(portBase + 1));
                } else {
                    fcgiProvider = make_shared<unix_fcgi_provider>(backend);
                }
                local_file_svc->register_fcgi(suffix, fcgiProvider);
                break;
            case 't':
                portBase = strchr(optarg, '=');
                if(!portBase) {
                    printf("Invalid MIME type definition - %s.\n", optarg);
                    return EXIT_FAILURE;
                }
                *portBase = 0;
                strcpy(suffix, optarg);
                local_file_svc->register_mimetype(suffix, string(portBase + 1));
                break;
            case 'p':
                strcpy(backend, optarg);
                portBase = strchr(backend, ':');
                if(portBase) {
                    *portBase = 0;
                    proxy_svc->append(backend, atoi(portBase + 1));
                } else {
                    proxy_svc->append(backend, 80);
                }
                break;
            case 'd':
                local_file_svc->add_defdoc_name(optarg);
                break;
            case 'l':
                logStream = new ofstream(optarg, ios_base::app | ios_base::out);
                break;
            case 'D':
                daemonize = true;
                break;
            default:
            case 'h':
                print_usage(argv[0]);
                return EXIT_FAILURE;
        }
    }
    if(daemonize) become_daemon();
    shared_ptr<http_service_chain> svcChain(new http_service_chain());
    try {
        if(useTLS) svcChain->append(make_shared<tls_filter_service>(302));
        if(daemonize && !logStream)
            logStream = new ofstream(fmt("/tmp/tinyhttpd-%d-access.log", getpid()));
        if(logStream)
            svcChain->append(make_shared<logger_service>(*logStream));
        else
            svcChain->append(make_shared<logger_service>(cout));
        svcChain->append(local_file_svc);
        if(proxy_svc->count() > 0)
            svcChain->append(proxy_svc);
        if(useTLS)
            server = make_shared<https_server>(ctx, svcChain);
        else
            server = make_shared<http_server>(svcChain);
        server->listen(bindAddr, port);
        if(!daemonize)
            printf("Service running at %s:%d.\n", bindAddr, port);
    }
    catch(runtime_error &ex) {
        printf("Failed to bind port %d: %s\n", port, ex.what());
    }
    signal_watcher watchint(SIGINT);
    signal_watcher watchterm(SIGTERM);
    signal(SIGPIPE, SIG_IGN);
    uv_run(uv_default_loop(), UV_RUN_DEFAULT);
    if(logStream) delete logStream;
    return EXIT_SUCCESS;
}