#include <xystream.h>
#include <xyhttpsvc.h>
#include <gtest/gtest.h>

#define TEST_BIND_PORT 19000

static void handle_walker(uv_handle_t* handle, void* arg) {
    *(int *)arg += 1;
}

TEST(IO, TcpStream) {
    bool checkpoint_finished = false;
    tcp_server server("127.0.0.1", TEST_BIND_PORT);
    server.serve([&checkpoint_finished] (shared_ptr<tcp_stream> strm) {
        auto request_decoder = make_shared<http_request::decoder>();
        auto request = strm->read<http_request>(request_decoder);
        ASSERT_EQ(request->method(), http_method::POST);

        auto content_decoder = make_shared<rest_decoder>(
                atoi(request->header("content-length")->c_str()));
        stream_buffer content_sb;
        while(content_decoder->more())
            content_sb.append(strm->read<string_message>(content_decoder));
        ASSERT_EQ(memcmp(content_sb.data(), "Hello world", 11), 0);
        ASSERT_EQ(content_sb.size(), 11);

        // return NULL when reached EOF
        ASSERT_EQ(strm->read(request_decoder), nullptr);

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
        tx->request->set_resource(make_shared<string>("/hello"));
        tx->forward_to("127.0.0.1", TEST_BIND_PORT);
    });
    chain->route<lambda_service>("/throw", [] (http_trx &tx) {
        throw runtime_error("expected error");
    });
    chain->route<lambda_service>("/large-data", [] (http_trx &tx) {
        for(int i = 0; i < 512; i++)
            tx->write("abcdefghAbcdefgh");
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

        req->set_resource(make_shared<string>("/hello"));
        client->write(req);
        auto resp = client->read<http_response>(response_decoder);
        ASSERT_EQ(resp->code(), 200);
        ASSERT_EQ(resp->header("Connection")->compare("keep-alive"), 0);
        auto content_decoder = make_shared<rest_decoder>(
                atoi(resp->header("Content-Length")->c_str()));
        while(content_decoder->more())
            sb_content.append(client->read(content_decoder));
        ASSERT_EQ(memcmp(sb_content.data(), "Hello world", sb_content.size()), 0);
        sb_content.pull(sb_content.size());

        req->set_resource(make_shared<string>("/forward"));
        client->write(req);
        resp = client->read<http_response>(response_decoder);
        ASSERT_EQ(resp->code(), 200);
        ASSERT_EQ(resp->header("Connection")->compare("keep-alive"), 0);
        content_decoder = make_shared<rest_decoder>(
                atoi(resp->header("Content-Length")->c_str()));
        while(content_decoder->more())
            sb_content.append(client->read(content_decoder));
        ASSERT_EQ(memcmp(sb_content.data(), "Hello world", sb_content.size()), 0);
        sb_content.pull(sb_content.size());

        // runtime_errors should be responded with 500 responses
        req->set_resource(make_shared<string>("/throw"));
        client->write(req);
        resp = client->read<http_response>(response_decoder);
        ASSERT_EQ(resp->code(), 500);
        ASSERT_EQ(resp->header("Connection")->compare("keep-alive"), 0);
        content_decoder = make_shared<rest_decoder>(
                atoi(resp->header("Content-Length")->c_str()));
        while(content_decoder->more()) client->read(content_decoder);

        req->set_resource(make_shared<string>("/large-data"));
        client->write(req);
        resp = client->read<http_response>(response_decoder);
        ASSERT_EQ(resp->code(), 200);
        ASSERT_FALSE(resp->header("Content-Encoding"));
        content_decoder = make_shared<rest_decoder>(
                atoi(resp->header("Content-Length")->c_str()));
        while(content_decoder->more())
            sb_content.append(client->read(content_decoder));
        ASSERT_EQ(sb_content.size(), 8192);
        sb_content.pull(sb_content.size());

        // Test gzip compression
        req->set_resource(make_shared<string>("/large-data"));
        req->set_header("Accept-Encoding", "deflate, gzip");
        client->write(req);
        resp = client->read<http_response>(response_decoder);
        ASSERT_EQ(resp->code(), 200);
        ASSERT_TRUE(resp->header("Content-Encoding"));
        content_decoder = make_shared<rest_decoder>(
                atoi(resp->header("Content-Length")->c_str()));
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