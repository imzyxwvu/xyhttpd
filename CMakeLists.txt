cmake_minimum_required(VERSION 3.8)
project(xyhttpd)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -rdynamic -g")

find_package(OpenSSL)

set(SOURCE_FILES src/main.cpp src/xystreambuffer.cpp src/xyhttp.h src/xyhttpapi.cpp src/xyhttprequest.cpp src/xyhttpresp.cpp src/xyhttpsvc.cpp src/xyhttp.cpp src/xyfcgi.cpp src/xyfiber.h src/xyfiber.cpp src/xystream.h src/xystream.cpp src/xylog.cpp src/xyhttptls.cpp)
add_executable(xyhttpd ${SOURCE_FILES})
target_link_libraries(xyhttpd uv ${OPENSSL_LIBRARIES})