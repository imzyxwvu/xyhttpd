#include <xyfiber.h>
#include <xyhttp.h>
#include <gtest/gtest.h>

TEST(Common, FiberMultilevel) {
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

TEST(Common, StreamBufferDecoding) {
    stream_buffer sb;

    // Test standard prepare & commit behaviour
    char *buf = sb.prepare(12);
    ASSERT_EQ(sb.size(), 0);
    memcpy(buf, "Hello world!", 11);
    sb.commit(11);
    ASSERT_EQ(sb.to_string()->compare("Hello world"), 0);

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
    ASSERT_EQ(request->method(), http_method::POST);
    ASSERT_EQ(request->path()->compare("/test"), 0);
    ASSERT_EQ(request->query()->compare("hello=world"), 0);
    ASSERT_EQ(atoi(request->header("content-length")->c_str()),sb.size());
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

TEST(Common, HttpResponse) {
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
    ASSERT_EQ(response->header("Server")->compare("xyhttpd/test"), 0);

    // Test Content-Length-based transfer
    auto content_decoder = make_shared<rest_decoder>(
            atoi(response->header("Content-Length")->c_str()));
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
    response->cookies.push_back(make_shared<string>("test3=23333"));
    sb.append(response);
    ASSERT_TRUE(response_decoder->decode(sb));
    ASSERT_EQ(sb.size(), 0);
    response = dynamic_pointer_cast<http_response>(response_decoder->msg());
    ASSERT_EQ(response->cookies.size(), 3);

    // Test chunked transfer encoding
    char test_chunk[] = "4\r\nTEST";
    sb.append(test_chunk, sizeof(test_chunk) - 1);
    auto chunk_decoder = make_shared<http_transfer_decoder>(
            response->header("Transfer-Encoding"));
    ASSERT_FALSE(chunk_decoder->decode(sb));
    sb.append("\r\n", 2);
    ASSERT_TRUE(chunk_decoder->decode(sb));
    ASSERT_EQ(sb.size(), 0);
    auto msg = dynamic_pointer_cast<string_message>(chunk_decoder->msg());
    ASSERT_EQ(memcmp(msg->data(), "TEST", 4), 0);
}
