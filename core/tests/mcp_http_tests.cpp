// MCP over HTTP: the server on a real loopback socket, as an MCP client sees
// it. One request per kind of answer, each expected status from the
// Streamable HTTP transport as zx_server serves it: 200 with the reply, 202
// for a notification, 404 off /mcp, 405 for anything but POST and DELETE,
// 400 for a body that is not JSON.

#include "engine.h"
#include "mcp_server.h"
#include "net.h"
#include "test_main.h"
#include "test_rom.h"

#include <nlohmann/json.hpp>

#include <string>
#include <thread>

using json = nlohmann::json;

namespace {

struct Reply {
    int status = 0;
    std::string body;
};

/// Sends one HTTP request on a fresh connection and reads the reply, its body
/// by Content-Length.
Reply exchange(uint16_t port, const std::string& method, const std::string& target, const std::string& body) {
    Reply reply;
    std::string error;
    ql::net::Socket sock = ql::net::connect("127.0.0.1", port, error);
    if (!sock.valid()) return reply;
    std::string request = method + " " + target + " HTTP/1.1\r\nHost: localhost\r\n"
                          + "Content-Type: application/json\r\nContent-Length: " + std::to_string(body.size())
                          + "\r\n\r\n" + body;
    sock.send_all(request);

    std::string data;
    char buffer[4096];
    size_t header_end = std::string::npos;
    size_t length = 0;
    for (;;) {
        if (header_end == std::string::npos) {
            header_end = data.find("\r\n\r\n");
            if (header_end != std::string::npos) {
                reply.status = std::stoi(data.substr(9, 3));
                size_t at = data.find("Content-Length: ");
                if (at != std::string::npos && at < header_end) length = std::stoul(data.substr(at + 16));
            }
        }
        if (header_end != std::string::npos && data.size() >= header_end + 4 + length) break;
        int n = sock.recv(buffer, sizeof buffer);
        if (n <= 0) break;
        data.append(buffer, size_t(n));
    }
    if (header_end != std::string::npos) reply.body = data.substr(header_end + 4, length);
    return reply;
}

} // namespace

TEST(mcp_over_http) {
    std::string error;
    CHECK(ql::net::startup(error));
    ql::Engine engine(qltest::counting_rom(), 128);
    ql::net::Listener listener;
    CHECK(listener.listen("127.0.0.1", 0, error));
    uint16_t port = listener.port();
    std::thread server([&engine, &listener] { ql::mcp::serve(engine, listener); });

    Reply r = exchange(port, "POST", "/mcp",
                       R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{}})");
    CHECK_EQ(r.status, 200);
    json reply = json::parse(r.body, nullptr, false);
    CHECK(!reply.is_discarded() && reply.at("result").at("serverInfo").at("name") == "ql_server");

    r = exchange(port, "POST", "/mcp", R"({"jsonrpc":"2.0","method":"notifications/initialized"})");
    CHECK_EQ(r.status, 202);
    r = exchange(port, "POST", "/elsewhere", "{}");
    CHECK_EQ(r.status, 404);
    r = exchange(port, "GET", "/mcp", "");
    CHECK_EQ(r.status, 405);
    r = exchange(port, "POST", "/mcp", "not json");
    CHECK_EQ(r.status, 400);

    listener.close();
    server.join();
}

RUN_TESTS()
