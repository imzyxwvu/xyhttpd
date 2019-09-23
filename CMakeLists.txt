cmake_minimum_required(VERSION 3.5)
project(xyhttpd)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -g")
include(GoogleTest OPTIONAL)

find_package(OpenSSL)
find_package(ZLIB)
find_library(GTEST_LIBRARIES NAMES gtest gtest_main)

include_directories(include)
set(HEADER_FILES
        include/xybuffer.h
        include/xycommon.h
        include/xyfcgi.h
        include/xyfiber.h
        include/xyhttp.h
        include/xyhttpsvc.h
        include/xyhttptls.h
        include/xystream.h
        include/xytlsstream.h
        include/xywebsocket.h
        )
set(SOURCE_FILES
        src/httpcore/xybase64.cpp
        src/httpcore/xybuffer.cpp
        src/httpcore/xyfcgi.cpp
        src/httpcore/xyfiber.cpp
        src/httpcore/xyhttp.cpp
        src/httpcore/xyhttpapi.cpp
        src/httpcore/xyhttpmsg.cpp
        src/httpcore/xyhttpsvc.cpp
        src/httpcore/xyhttptls.cpp
        src/httpcore/xystream.cpp
        src/httpcore/xytlsstream.cpp
        )
add_library(iocore ${SOURCE_FILES} ${HEADER_FILES})
target_link_libraries(iocore uv ${ZLIB_LIBRARIES} ${OPENSSL_LIBRARIES})

add_executable(tinyhttpd src/tinyhttpd/tinyhttpd.cpp)
target_link_libraries(tinyhttpd iocore)

if(GTEST_LIBRARIES)
    enable_testing()

    add_executable(test-io test/test-io.cpp)
    target_link_libraries(test-io iocore ${GTEST_LIBRARIES} gtest_main pthread)
    gtest_discover_tests(test-io)
else()
    message(STATUS "Note: "
            "Google Test is not found on your system. Unit tests are skipped.")
endif()