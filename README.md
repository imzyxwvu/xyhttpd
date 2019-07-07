# xyhttpd C++ Web IO 框架

xyhttpd 是一套 C++ 非阻塞的 Web IO 框架，目前基于 libuv 实现，目标平台为 Linux、macOS 和 Windows（基础功能）。xyhttpd 的目的是为 C++ 网络服务开发提供便利，基于如下设计思想，xyhttpd 可以实现网络协议的高效实现与业务逻辑的自然描述：

* 大规模使用 shared_ptr 和 virtual：简化内存管理，并尽最大可能保证类的多态性，使框架具有极高的可扩展性
* Fiber 驱动：可以在避免线程同步开销以及资源竞争的基础上，更加自然地描述业务逻辑
* decoder 模式网络 IO 处理：实现协议的语法与语义分离，对通信报文的解析和封装将被封装到 decoder 和 message 类中，业务执行绪中的代码只需关注业务逻辑的处理
* TLS 支持：提供 tcp_stream 和 tls_stream，tls_stream 对象初始化完成后可直接作 tcp_stream 对象使用，业务代码无需关注过多 TLS 相关的底层细节
* 多种现成 HTTP 功能： 静态文件处理、默认页面、反向代理、FastCGI、HTTPS等

## Build

    cmake . && make -j
    
本项目使用 cmake 编译，编译成功后可以得到 libiocore.a（xyhttpd 核心库）和 tinyhttpd（Demo 程序）。

## Demo: tinyhttpd

[tinyhttpd](https://github.com/imzyxwvu/xyhttpd/blob/master/src/tinyhttpd/tinyhttpd.cpp) 是一个基于 xyhttpd 框架的轻量级 HTTP 服务器。使用方法十分简单：

    Usage: ./tinyhttpd [-h] [-r htdocs] [-b 0.0.0.0:8080] [-d index.php]
       [-f FcgiProvider] [-p 127.0.0.1:90]

       -h   Show help information
       -r   Set document root
       -b   Set bind address and port
       -d   Add default document search name
       -f   Add FastCGI suffix and handler
       -p   Add proxy pass backend service
       
比如要在 8090 端口提供位于 /var/www/blog 的 PHP 站点，只需如下一条命令（假定系统中 PHP-FPM 已在运行）：

    ./tinyhttpd -r /var/www/blog -b 0.0.0.0:8090 -d index.php -f php=/var/sock/php-fpm.sock
    
## Getting started?

更多相关资料可以从 Wiki 中找到~

* [实现一个简单的 HTTP Server](https://github.com/imzyxwvu/xyhttpd/wiki/Implementing-HTTP-Server)
* [编写自定义的 HTTP Service](https://github.com/imzyxwvu/xyhttpd/wiki/Creating-HTTP-Service)
