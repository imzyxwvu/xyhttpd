cmake_minimum_required(VERSION 3.8)
project(xyhttpd)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -rdynamic -g")
include_directories(include)

find_package(OpenSSL)

set(SOURCE_FILES src/httpcore/xystreambuffer.cpp include/xyhttp.h src/httpcore/xyhttpapi.cpp src/httpcore/xyhttprequest.cpp src/httpcore/xyhttpresp.cpp src/httpcore/xyhttpsvc.cpp src/httpcore/xyhttp.cpp src/httpcore/xyfcgi.cpp include/xyfiber.h src/httpcore/xyfiber.cpp include/xystream.h src/httpcore/xystream.cpp src/httpcore/xylog.cpp src/httpcore/xyhttptls.cpp)
add_library(iocore ${SOURCE_FILES})
target_link_libraries(iocore uv ${OPENSSL_LIBRARIES})

add_executable(xyhttpd src/main.cpp)
target_link_libraries(xyhttpd iocore)