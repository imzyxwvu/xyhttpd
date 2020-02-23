cmake_minimum_required(VERSION 3.5)
project(xyhttpd)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -g")
include(GoogleTest OPTIONAL)

find_package(OpenSSL)
find_package(ZLIB)
find_package(PkgConfig)

find_library(GTEST_LIBRARIES NAMES gtest gtest_main)
pkg_search_module(LIBUV REQUIRED libuv)
pkg_search_module(LUAJIT REQUIRED luajit)

include_directories(include)
add_compile_options(-fPIC -fsanitize=leak)
set(HEADER_FILES
        include/xycommon.h
        include/xyfcgi.h
        include/xyfiber.h
        include/xyhttp.h
        include/xyhttpsvc.h
        include/xyhttptls.h
        include/xystream.h
        )
set(SOURCE_FILES
        src/httpcore/xybase64.cpp
        src/httpcore/xyfcgi.cpp
        src/httpcore/xyfiber.cpp
        src/httpcore/xyhttp.cpp
        src/httpcore/xyhttpapi.cpp
        src/httpcore/xyhttpsvc.cpp
        src/httpcore/xyhttptls.cpp
        src/httpcore/xystream.cpp
        )
add_library(iocore ${SOURCE_FILES} ${HEADER_FILES})
target_link_libraries(iocore ${LIBUV_LIBRARIES} ${ZLIB_LIBRARIES} ${OPENSSL_LIBRARIES})

add_executable(tinyhttpd src/tinyhttpd/tinyhttpd.cpp)
target_link_libraries(tinyhttpd iocore)

add_compile_options(${LUAJIT_CFLAGS})

add_library(liocore SHARED src/luabridge/liocore.cpp src/luabridge/liocore.h src/luabridge/lhttptrx.cpp)
set_target_properties(liocore PROPERTIES PREFIX "")
target_link_libraries(liocore iocore)

# Delegate for debugging Lua-bridge in CLion
add_executable(ldelegate EXCLUDE_FROM_ALL src/luabridge/delegate.c)
target_link_libraries(ldelegate ${LUAJIT_LIBRARIES})

if(GTEST_LIBRARIES)
    enable_testing()

    add_executable(test-io test/test-io.cpp)
    target_link_libraries(test-io iocore ${GTEST_LIBRARIES} gtest_main pthread)
    gtest_discover_tests(test-io)
else()
    message(STATUS "Note: "
            "Google Test is not found on your system. Unit tests are skipped.")
endif()

install(TARGETS tinyhttpd liocore
        RUNTIME DESTINATION bin
        LIBRARY DESTINATION lib/lua/5.1)