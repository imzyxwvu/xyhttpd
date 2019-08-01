cmake_minimum_required(VERSION 3.8)
project(xyhttpd)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -rdynamic -g")
include_directories(include)

find_package(OpenSSL)
find_package(ZLIB)

set(SOURCE_FILES
        src/httpcore/xystreambuffer.cpp
        src/httpcore/xybase64.cpp
        src/httpcore/xymsgsink.cpp
        src/httpcore/xyhttpapi.cpp
        src/httpcore/xyhttprequest.cpp
        src/httpcore/xyhttpresp.cpp
        src/httpcore/xyhttpsvc.cpp
        src/httpcore/xyhttp.cpp
        src/httpcore/xyfcgi.cpp
        src/httpcore/xyfiber.cpp
        src/httpcore/xystream.cpp
        src/httpcore/xylog.cpp
        src/httpcore/xyhttptls.cpp
        src/httpcore/xywebsocket.cpp
        )
add_library(iocore ${SOURCE_FILES})
target_link_libraries(iocore uv ${ZLIB_LIBRARIES} ${OPENSSL_LIBRARIES})

add_executable(xyhttpd src/main.cpp)
target_link_libraries(xyhttpd iocore)

add_executable(tinyhttpd src/tinyhttpd/tinyhttpd.cpp)
target_link_libraries(tinyhttpd iocore)

add_executable(iomaster src/iomaster/iomaster.cpp src/iomaster/iomnbd.cpp)
target_link_libraries(iomaster iocore)