#include "xyhttpsvc.h"
#include "xyhttptls.h"
#include <getopt.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <iostream>
#include <fstream>

using namespace std;

shared_ptr<http_server> server;

class signal_watcher {
public:
    explicit signal_watcher(int signo) {
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
           "Usage: %s [-Dh] [-r htdocs] [-b 0.0.0.0:8080] [-d index.php]\n"
           "       [-f FcgiProvider] [-p 127.0.0.1:90]\n\n", progname);
    puts("   -h\tShow this help information");
    puts("   -r\tSet path to document root directory. If not set, current working ");
    puts("     \tdirectory is used for convenience file sharing.");
    puts("   -b\tSet bind address and port");
    puts("   -s\tEnable TLS and use provided X509 certificate chain : PEM key pair");
    puts("   -d\tAdd default document search name.");
    puts("   -f\tAdd dynamic page suffix and its FastCGI handler.");
    puts("     \tTCP IP:port pair or UNIX domain socket path is accepted.");
    puts("   -p\tAdd proxy pass backend service. If multiple services are specified,");
    puts("     \tthey will be used in a round-robin machanism for load balancing.");
    puts("   -l\tSpecify HTTP access log file name.");
    puts("   -D\tBecome a background daemon process.");
    puts("");
}

#ifndef _WIN32
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
#endif

static void register_mimetypes(P<local_file_service> &svc) {
    svc->register_mimetype("mid midi kar", "audio/midi");
    svc->register_mimetype("aac f4a f4b m4a", "audio/mp4");
    svc->register_mimetype("mp3", "audio/mpeg");
    svc->register_mimetype("oga ogg opus", "audio/ogg");
    svc->register_mimetype("ra", "audio/x-realaudio");
    svc->register_mimetype("wav", "audio/x-wav");
    svc->register_mimetype("bmp", "image/bmp");
    svc->register_mimetype("gif", "image/gif");
    svc->register_mimetype("jpeg jpg", "image/jpeg");
    svc->register_mimetype("png", "image/png");
    svc->register_mimetype("svg svgz", "image/svg+xml");
    svc->register_mimetype("tif tiff", "image/tiff");
    svc->register_mimetype("wbmp", "image/vnd.wap.wbmp");
    svc->register_mimetype("webp", "image/webp");
    svc->register_mimetype("ico cur", "image/x-icon");
    svc->register_mimetype("jng", "image/x-jng");
    svc->register_mimetype("js", "application/javascript");
    svc->register_mimetype("json", "application/json");
    svc->register_mimetype("webapp", "application/x-web-app-manifest+json");
    svc->register_mimetype("manifest appcache", "text/cache-manifest");
    svc->register_mimetype("doc", "application/msword");
    svc->register_mimetype("xls", "application/vnd.ms-excel");
    svc->register_mimetype("ppt", "application/vnd.ms-powerpoint");
    svc->register_mimetype("docx", "application/vnd.openxmlformats-officedocument.wordprocessingml.document");
    svc->register_mimetype("xlsx", "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet");
    svc->register_mimetype("pptx", "application/vnd.openxmlformats-officedocument.presentationml.presentation");
    svc->register_mimetype("3gpp 3gp", "video/3gpp");
    svc->register_mimetype("mp4 m4v f4v f4p", "video/mp4");
    svc->register_mimetype("mpeg mpg", "video/mpeg");
    svc->register_mimetype("ogv", "video/ogg");
    svc->register_mimetype("mov", "video/quicktime");
    svc->register_mimetype("webm", "video/webm");
    svc->register_mimetype("flv", "video/x-flv");
    svc->register_mimetype("mng", "video/x-mng");
    svc->register_mimetype("asx asf", "video/x-ms-asf");
    svc->register_mimetype("wmv", "video/x-ms-wmv");
    svc->register_mimetype("avi", "video/x-msvideo");
    svc->register_mimetype("atom rdf rss xml", "application/xml");

    svc->register_mimetype("woff", "application/font-woff");
    svc->register_mimetype("woff2", "application/font-woff2");
    svc->register_mimetype("eot", "application/vnd.ms-fontobject");
    svc->register_mimetype("ttc ttf", "application/x-font-ttf");
    svc->register_mimetype("otf", "font/opentype");

    svc->register_mimetype("jar war ear", "application/java-archive");
    svc->register_mimetype("hqx", "application/mac-binhex40");
    svc->register_mimetype("pdf", "application/pdf");
    svc->register_mimetype("ps eps ai", "application/postscript");
    svc->register_mimetype("rtf", "application/rtf");
    svc->register_mimetype("wmlc", "application/vnd.wap.wmlc");
    svc->register_mimetype("xhtml", "application/xhtml+xml");
    svc->register_mimetype("kml", "application/vnd.google-earth.kml+xml");
    svc->register_mimetype("kmz", "application/vnd.google-earth.kmz");
    svc->register_mimetype("7z", "application/x-7z-compressed");
    svc->register_mimetype("crx", "application/x-chrome-extension");
    svc->register_mimetype("oex", "application/x-opera-extension");
    svc->register_mimetype("xpi", "application/x-xpinstall");
    svc->register_mimetype("cco", "application/x-cocoa");
    svc->register_mimetype("jardiff", "application/x-java-archive-diff");
    svc->register_mimetype("jnlp", "application/x-java-jnlp-file");
    svc->register_mimetype("run", "application/x-makeself");
    svc->register_mimetype("pl pm", "application/x-perl");
    svc->register_mimetype("prc pdb", "application/x-pilot");
    svc->register_mimetype("rar", "application/x-rar-compressed");
    svc->register_mimetype("rpm", "application/x-redhat-package-manager");
    svc->register_mimetype("sea", "application/x-sea");
    svc->register_mimetype("swf", "application/x-shockwave-flash");
    svc->register_mimetype("sit", "application/x-stuffit");
    svc->register_mimetype("tcl tk", "application/x-tcl");
    svc->register_mimetype("der pem crt", "application/x-x509-ca-cert");
    svc->register_mimetype("torrent", "application/x-bittorrent");
    svc->register_mimetype("zip", "application/zip");
    svc->register_mimetype("bin exe dll", "application/octet-stream");
    svc->register_mimetype("deb dmg iso img", "application/octet-stream");
    svc->register_mimetype("msi msp msm", "application/octet-stream");

    svc->register_mimetype("css", "text/css");
    svc->register_mimetype("html htm shtml", "text/html");
    svc->register_mimetype("mml", "text/mathml");
    svc->register_mimetype("txt", "text/plain");
    svc->register_mimetype("jad", "text/vnd.sun.j2me.app-descriptor");
    svc->register_mimetype("wml", "text/vnd.wap.wml");
    svc->register_mimetype("vtt", "text/vtt");
    svc->register_mimetype("htc", "text/x-component");
    svc->register_mimetype("vcf", "text/x-vcard");
}

int main(int argc, char *argv[])
{
    char bindAddr[PATH_MAX] = "0.0.0.0";
    int port = 8080;
    auto fileService = make_shared<local_file_service>(pwd());
    auto proxyService = make_shared<proxy_pass_service>();
    char *portBase;
    char suffix[16];
    char backend[PATH_MAX];
    int opt;
    shared_ptr<tls_context> ctx;
    bool daemonize = false;
    unique_ptr<ostream> logStream;
    while ((opt = getopt(argc, argv, "r:b:f:d:p:t:s:l:Dh")) != -1) {
        switch(opt) {
            case 'r':
                fileService->set_document_root(optarg);
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
                if(ctx) {
                    printf("Duplicate -s.\n");
                    return EXIT_FAILURE;
                }
                portBase = strchr(optarg, ':');
                if(!portBase) {
                    printf("TLS private key not specified.\n");
                    return EXIT_FAILURE;
                }
                ctx = make_shared<tls_context>();
                *portBase = 0;
                ctx->use_certificate(optarg, portBase + 1);
                break;
            case 'f': {
                portBase = strchr(optarg, '=');
                if(!portBase) {
                    printf("Invalid FastCGI handler - %s.\n", optarg);
                    return EXIT_FAILURE;
                }
                *portBase = 0;
                strcpy(suffix, optarg);
                strcpy(backend, portBase + 1);
                portBase = strchr(backend, ':');
                if(portBase && backend[0] != '/') {
                    *portBase = 0;
                    fileService->register_fcgi(suffix,
                            make_shared<tcp_fcgi_provider>(backend, atoi(portBase + 1)));
                } else {
                    fileService->register_fcgi(suffix, make_shared<unix_fcgi_provider>(backend));
                }
                break;
            }
            case 'p':
                strcpy(backend, optarg);
                portBase = strchr(backend, ':');
                if(portBase) {
                    *portBase = 0;
                    proxyService->append(backend, atoi(portBase + 1));
                } else {
                    proxyService->append(backend, 80);
                }
                break;
            case 'd':
                fileService->add_default_name(optarg);
                break;
            case 'l':
                logStream.reset(new ofstream(optarg, ios_base::app | ios_base::out));
                break;
            case 'D':
                daemonize = true;
                if(!logStream) {
                    string logFile = fmt("/tmp/tinyhttpd-%d-access.log", getpid());
                    logStream.reset(new ofstream(logFile, ios_base::out));
                }
                break;
            default:
            case 'h':
                print_usage(argv[0]);
                return EXIT_FAILURE;
        }
    }
#ifndef _WIN32
    if(daemonize) become_daemon();
    signal(SIGPIPE, SIG_IGN);
#endif
    register_mimetypes(fileService);
    try {
        auto svcChain = make_shared<http_service_chain>();
        if(ctx) svcChain->append<tls_filter_service>(302);
        svcChain->append<logger_service>(logStream ? logStream.get() : &cout);
        svcChain->append(fileService);
        if(proxyService->count() > 0) svcChain->append(proxyService);
        server = ctx ? make_shared<https_server>(ctx, svcChain) : make_shared<http_server>(svcChain);
        server->listen(bindAddr, port);
        if(!daemonize) printf("Service running at %s:%d.\n", bindAddr, port);
    }
    catch(runtime_error &ex) {
        printf("Failed to bind port %d: %s\n", port, ex.what());
    }
    signal_watcher watchint(SIGINT);
    signal_watcher watchterm(SIGTERM);
    uv_run(uv_default_loop(), UV_RUN_DEFAULT);
    return EXIT_SUCCESS;
}