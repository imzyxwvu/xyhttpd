#include <xystream.h>
#include <xyhttpsvc.h>
#include <gtest/gtest.h>

using namespace std;

#define TEST_BIND_PORT 19000

static void handle_walker(uv_handle_t* handle, void* arg) {
    *(int *)arg += 1;
}

TEST(IO, FiberMultilevel) {
    bool checkpoints[10];
    shared_ptr<fiber> f1, f2, f3;
    for(int i = 0; i < sizeof(checkpoints) / sizeof(bool); i++)
        checkpoints[i] = false;

    f1 = fiber::launch([&checkpoints, &f1] () {
        checkpoints[0] = true;
        ASSERT_NO_THROW(fiber::yield());
        ASSERT_EQ(fiber::current(), f1);
        ASSERT_ANY_THROW(f1->resume()); // Test self resume
        checkpoints[3] = true;
        ASSERT_ANY_THROW(fiber::yield());
        ASSERT_TRUE(checkpoints[4]);
        checkpoints[5] = true;
    });
    ASSERT_TRUE(checkpoints[0]);

    f2 = fiber::launch([&checkpoints, f1] () {
        checkpoints[1] = true;
        fiber::yield();
        f1->resume(); // Test multi-level resume
        ASSERT_TRUE(checkpoints[3]);
        checkpoints[4] = true;
        fiber::yield();
        f1->raise("expected error"); // Test remote throw
        ASSERT_TRUE(checkpoints[5]);
        checkpoints[6] = true;
    });
    ASSERT_TRUE(checkpoints[1]);

    f3 = fiber::launch([&checkpoints, f1, f2] () {
        checkpoints[2] = true;
        fiber::yield();
        f2->resume();
        ASSERT_TRUE(checkpoints[6]);
        checkpoints[7] = true;
    });
    ASSERT_TRUE(checkpoints[2]);

    ASSERT_TRUE(!fiber::current());
    f2->resume();
    ASSERT_TRUE(checkpoints[4]);
    f3->resume();
    ASSERT_TRUE(checkpoints[7]);
    ASSERT_ANY_THROW(f1->resume());

    // Check resource release
    ASSERT_EQ(f3.use_count(), 1);
    f3.reset();
    ASSERT_EQ(f2.use_count(), 1);
    f2.reset();
    ASSERT_EQ(f1.use_count(), 1);
    f1.reset();
}

TEST(IO, StreamBufferDecoding) {
    stream_buffer sb;

    // Test standard prepare & commit behaviour
    char *buf = sb.prepare(12);
    ASSERT_EQ(sb.size(), 0);
    memcpy(buf, "Hello world!", 11);
    sb.commit(11);
    ASSERT_TRUE(sb.dump() == "Hello world");

    // Test malformed HTTP request decoding behaviour
    auto request_decoder = make_shared<http_request::decoder>();
    ASSERT_ANY_THROW(request_decoder->decode(sb));
    ASSERT_EQ(sb.size(), 11);

    // Test pull & detach behaviours
    sb.pull(6);
    ASSERT_EQ(sb.size(), 5);
    buf = sb.detach();
    ASSERT_EQ(sb.size(), 0);
    ASSERT_EQ(memcmp(buf, "world", 5), 0);
    free(buf);

    // Test partial HTTP request decoding behaviour
    char test_request_part1[] =
            "POST /test?hello=world HTTP/1.1\r\n"
            "Host: 0xspot.com\r\n"
            "User-Agent: Mozilla/5.0 ";
    sb.append(test_request_part1, sizeof(test_request_part1) - 1);
    ASSERT_EQ(sb.size(), sizeof(test_request_part1) - 1);
    ASSERT_FALSE(request_decoder->decode(sb));

    // Test entire HTTP request decoding behaviour
    char test_request_part2[] = "(test_request)\r\n"
                                "Content-Length: 4\r\n\r\nTEST";
    sb.append(test_request_part2, sizeof(test_request_part2) - 1);
    ASSERT_EQ(sb.size(),
              sizeof(test_request_part1) + sizeof(test_request_part2) - 2);
    ASSERT_TRUE(request_decoder->decode(sb));

    auto request = dynamic_pointer_cast<http_request>(request_decoder->msg());
    ASSERT_EQ(request->method, "POST");
    ASSERT_TRUE(request->path() == "/test");
    ASSERT_TRUE(request->query() =="hello=world");
    ASSERT_EQ(atoi(request->header("content-length")),sb.size());
    ASSERT_TRUE(request->header_include("user-agent", "test_request"));
    sb.pull(sb.size());

    // Test request modification and serialization
    request->set_header("host", "example.com");
    request->delete_header("content-length");
    sb.append(request);
    ASSERT_TRUE(request_decoder->decode(sb));
    ASSERT_EQ(sb.size(), 0);
    request = dynamic_pointer_cast<http_request>(request_decoder->msg());
    ASSERT_EQ(request->header("content-length"), nullptr);
    ASSERT_TRUE(request->header_include("user-agent", "test_request"));
}

TEST(IO, HttpResponse) {
    stream_buffer sb;
    char test_response[] = "HTTP/1.1 200 OK\r\n"
                           "Content-Length: 11\r\n" // Value used by following test
                           "Server: xyhttpd/test\r\n"
                           "Set-Cookie: test1=hello\r\n"
                           "Set-Cookie: test2=world\r\n\r\n";
    sb.append(test_response, sizeof(test_response) - 1);
    ASSERT_EQ(memcmp(test_response, sb.data(), sb.size()), 0);

    // Test HTTP response decoding
    auto response_decoder = make_shared<http_response::decoder>();
    ASSERT_TRUE(response_decoder->decode(sb));
    ASSERT_EQ(sb.size(), 0);
    auto response = dynamic_pointer_cast<http_response>(response_decoder->msg());
    ASSERT_EQ(response->code(), 200);
    ASSERT_TRUE(response->header("Server") == "xyhttpd/test");

    // Test Content-Length-based transfer
    auto content_decoder = make_shared<http_transfer_decoder>(response);
    stream_buffer content_sb;
    ASSERT_FALSE(content_decoder->decode(sb));
    sb.append("Hello ", 6);
    ASSERT_TRUE(content_decoder->decode(sb));
    ASSERT_EQ(sb.size(), 0);
    content_sb.append(content_decoder->msg());
    ASSERT_TRUE(content_decoder->more());
    sb.append("world!", 6);
    ASSERT_TRUE(content_decoder->decode(sb));
    ASSERT_EQ(sb.size(), 1);
    content_sb.append(content_decoder->msg());
    ASSERT_FALSE(content_decoder->more());
    sb.pull(sb.size());
    ASSERT_EQ(memcmp(content_sb.data(), "Hello world", 11), 0);
    ASSERT_EQ(content_sb.size(), 11);

    // Test HTTP response modification
    response->delete_header("Content-Length");
    response->set_header("Transfer-Encoding", "chunked");
    response->cookies.push_back("test3=23333");
    sb.append(response);
    ASSERT_TRUE(response_decoder->decode(sb));
    ASSERT_EQ(sb.size(), 0);
    response = dynamic_pointer_cast<http_response>(response_decoder->msg());
    ASSERT_EQ(response->cookies.size(), 3);

    // Test chunked transfer encoding
    char test_chunk[] = "4\r\nTEST";
    sb.append(test_chunk, sizeof(test_chunk) - 1);
    auto chunk_decoder = make_shared<http_transfer_decoder>(response);
    ASSERT_FALSE(chunk_decoder->decode(sb));
    sb.append("\r\n", 2);
    ASSERT_TRUE(chunk_decoder->decode(sb));
    ASSERT_EQ(sb.size(), 0);
    auto msg = dynamic_pointer_cast<string_message>(chunk_decoder->msg());
    ASSERT_EQ(memcmp(msg->data(), "TEST", 4), 0);
}

TEST(IO, TcpStream) {
    bool checkpoint_finished = false;
    tcp_server server("127.0.0.1", TEST_BIND_PORT);
    server.serve([&checkpoint_finished] (shared_ptr<tcp_stream> strm) {
        auto request_decoder = make_shared<http_request::decoder>();
        auto request = strm->read<http_request>(request_decoder);
        ASSERT_EQ(request->method, "POST");

        auto content_decoder = make_shared<string_decoder>(
                atoi(request->header("content-length")));
        stream_buffer content_sb;
        while(content_decoder->more())
            content_sb.append(strm->read<string_message>(content_decoder));
        ASSERT_EQ(memcmp(content_sb.data(), "Hello world", 11), 0);
        ASSERT_EQ(content_sb.size(), 11);

        // return NULL when reached EOF
        try {
            strm->read(request_decoder);
            ASSERT_FALSE(true);
        }
        catch(runtime_error &ex) {
            ASSERT_TRUE(strstr(ex.what(), "end of file"));
        }

        auto response = make_shared<http_response>(200);
        response->set_header("Connection", "close");
        strm->write(response);
        strm->write("JUNK DATA");
        strm->shutdown();
        checkpoint_finished = true;
    });

    // Test pipe
    tcp_server pipe_server("127.0.0.1", 19001);
    pipe_server.serve([] (shared_ptr<stream> strm) {
       auto remote = make_shared<tcp_stream>();
       remote->connect("127.0.0.1", TEST_BIND_PORT);
       strm->pipe(remote);
       remote->pipe(strm);
       ASSERT_ANY_THROW(remote->pipe(strm));
    });

    // Client fiber
    fiber::launch([] () {
        auto strm = make_shared<tcp_stream>();
        strm->connect("127.0.0.1", 19001);

        string test_request_part1 =
                "POST /test?hello=world HTTP/1.1\r\n"
                "Connection: close\r\n"
                "Host: test-host\r\n"
                "User-Agent: Mozilla/5.0 ";
        strm->write(test_request_part1);

        string test_request_part2 = "(test_request)\r\n"
                                    "Content-Length: 11\r\n\r\nHello ";
        strm->write(test_request_part2);

        strm->write("world");
        strm->shutdown();

        auto response = strm->read<http_response>(
                make_shared<http_response::decoder>());
        ASSERT_EQ(response->code(), 200);
        ASSERT_ANY_THROW(strm->read<http_response>(
                make_shared<http_response::decoder>()));
        uv_stop(uv_default_loop());
    });
    uv_run(uv_default_loop(), UV_RUN_DEFAULT);
    ASSERT_TRUE(checkpoint_finished);
    // Give fibers a chance to finish
    uv_run(uv_default_loop(), UV_RUN_NOWAIT);

    // Check resource release
    int handle_count = 0;
    uv_walk(uv_default_loop(), handle_walker, &handle_count);
    ASSERT_EQ(handle_count, 2); // Should be the two tcp servers
}

TEST(IO, TcpConnectFail) {
    bool checkpoint_finished = false;
    fiber::launch([&checkpoint_finished] () {
       auto client = make_shared<tcp_stream>();
       ASSERT_ANY_THROW(client->connect("127.0.0.1", 19002));
       checkpoint_finished = true;
    });
    uv_run(uv_default_loop(), UV_RUN_DEFAULT);
    ASSERT_TRUE(checkpoint_finished);
}

TEST(IO, HttpServer) {
    bool checkpoint_forward = false;
    auto chain = make_shared<http_service_chain>();
    http_server server(chain);
    chain->route<lambda_service>("/hello", [&checkpoint_forward] (http_trx &tx) {
        if(tx->request->header("x-forwarded-for"))
            checkpoint_forward = true;
        tx->write("Hello world");
        tx->finish();
    });
    chain->route<lambda_service>("/forward", [] (http_trx &tx) {
        tx->request->set_resource("/hello");
        tx->forward_to("127.0.0.1", TEST_BIND_PORT);
    });
    chain->route<lambda_service>("/throw", [] (http_trx &tx) {
        throw runtime_error("expected error");
    });
    chain->route<lambda_service>("/large-data", [] (http_trx &tx) {
        for(int i = 0; i < 16384; i++)
            tx->write("abcdefghAbcdefghabcdEfghABCDEFGH");
        tx->finish();
    });
    server.listen("127.0.0.1", TEST_BIND_PORT);

    bool checkpoint_finished = false;
    fiber::launch([&checkpoint_finished] () {
        auto client = make_shared<tcp_stream>();
        client->connect("127.0.0.1", TEST_BIND_PORT);
        auto req = make_shared<http_request>();
        auto response_decoder = make_shared<http_response::decoder>();
        stream_buffer sb_content;
        req->set_header("Connection", "keep-alive");
        req->set_header("Host", "localhost");

        req->set_resource("/hello");
        client->write(req);
        auto resp = client->read<http_response>(response_decoder);
        ASSERT_EQ(resp->code(), 200);
        ASSERT_TRUE(resp->header("Connection") == "keep-alive");
        auto content_decoder = make_shared<http_transfer_decoder>(resp);
        while(content_decoder->more())
            sb_content.append(client->read(content_decoder));
        ASSERT_EQ(memcmp(sb_content.data(), "Hello world", sb_content.size()), 0);
        sb_content.pull(sb_content.size());

        req->set_resource("/forward");
        client->write(req);
        resp = client->read<http_response>(response_decoder);
        ASSERT_EQ(resp->code(), 200);
        ASSERT_TRUE(resp->header("Connection") == "keep-alive");
        content_decoder = make_shared<http_transfer_decoder>(resp);
        while(content_decoder->more())
            sb_content.append(client->read(content_decoder));
        ASSERT_EQ(memcmp(sb_content.data(), "Hello world", sb_content.size()), 0);
        sb_content.pull(sb_content.size());

        // runtime_errors should be responded with 500 responses
        req->set_resource("/throw");
        client->write(req);
        resp = client->read<http_response>(response_decoder);
        ASSERT_EQ(resp->code(), 500);
        ASSERT_TRUE(resp->header("Connection") == "keep-alive");
        content_decoder = make_shared<http_transfer_decoder>(resp);
        while(content_decoder->more()) client->read(content_decoder);

        // Test chunked Transfer-Encoding
        req->set_resource("/large-data");
        client->write(req);
        resp = client->read<http_response>(response_decoder);
        ASSERT_EQ(resp->code(), 200);
        ASSERT_FALSE(resp->header("Content-Encoding"));
        ASSERT_TRUE(resp->header("Transfer-Encoding"));
        content_decoder = make_shared<http_transfer_decoder>(resp);
        while(content_decoder->more())
            sb_content.append(client->read(content_decoder));
        ASSERT_EQ(sb_content.size(), 16384 * 32);
        sb_content.pull(sb_content.size());

        // Test gzip compression
        req->set_resource("/large-data");
        req->set_header("Accept-Encoding", "deflate, gzip");
        client->write(req);
        resp = client->read<http_response>(response_decoder);
        ASSERT_EQ(resp->code(), 200);
        ASSERT_TRUE(resp->header("Content-Encoding"));
        content_decoder = make_shared<http_transfer_decoder>(resp);
        while(content_decoder->more())
            sb_content.append(client->read(content_decoder));
        ASSERT_LT(sb_content.size(), 8192);
        sb_content.pull(sb_content.size());

        uv_stop(uv_default_loop());
        checkpoint_finished = true;
    });
    uv_run(uv_default_loop(), UV_RUN_DEFAULT);
    ASSERT_TRUE(checkpoint_finished);
    ASSERT_TRUE(checkpoint_forward);
    // Give fibers a chance to finish
    uv_run(uv_default_loop(), UV_RUN_NOWAIT);

    // Check resource release
    int handle_count = 0;
    uv_walk(uv_default_loop(), handle_walker, &handle_count);
    ASSERT_EQ(handle_count, 1); // Should be the two tcp servers
}