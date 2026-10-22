// MCP, at its message boundary: a JSON-RPC message in, the reply out, with no
// sockets. The machine behind it runs a hand-built ROM, and what each tool
// must answer is worked out from that program.
//
// Tool results follow zx_server's: text content carrying JSON, a failure as a
// result with isError rather than a protocol error, the screen as a base64
// PNG. Addresses are written as six hex digits (docs/plan.md).

#include "base64.h"
#include "engine.h"
#include "mcp_server.h"
#include "test_main.h"
#include "test_rom.h"

#include <nlohmann/json.hpp>

#include <cstdio>
#include <set>
#include <string>

using json = nlohmann::json;
using qltest::counting_rom;

namespace {

json request(int id, const std::string& method, const json& params = json::object()) {
    return json{{"jsonrpc", "2.0"}, {"id", id}, {"method", method}, {"params", params}};
}

json send(ql::Engine& engine, const json& message) {
    bool handled = false;
    json reply = ql::mcp::handle_message(engine, message, handled);
    CHECK(handled);
    return reply;
}

} // namespace

// initialize names the server and the protocol revision zx_server speaks;
// tools/list offers exactly this stage's tools.
TEST(initialize_and_tools_list) {
    ql::Engine engine(counting_rom(), 128);
    json reply = send(engine, request(1, "initialize"));
    CHECK_EQ(reply.at("id").get<int>(), 1);
    CHECK_EQ(reply.at("result").at("protocolVersion").get<std::string>(), std::string("2025-06-18"));
    CHECK_EQ(reply.at("result").at("serverInfo").at("name").get<std::string>(), std::string("ql_server"));
    CHECK(reply.at("result").at("capabilities").contains("tools"));

    reply = send(engine, request(2, "tools/list"));
    std::set<std::string> names;
    for (const json& tool : reply.at("result").at("tools")) names.insert(tool.at("name").get<std::string>());
    const std::set<std::string> expected = {"run",       "pause",       "step",       "reset",
                                            "get_state", "read_memory", "get_screen", "server_info"};
    CHECK(names == expected);
}

namespace {

json call(ql::Engine& engine, int id, const std::string& name, const json& arguments = json::object()) {
    return send(engine, request(id, "tools/call", json{{"name", name}, {"arguments", arguments}}));
}

/// A successful tool result's JSON, from its text content. `at` rather than
/// `[]` throughout: on a missing key it throws, which fails the test, where
/// `[]` on a const json asserts -- and in a Debug build an assertion is a
/// dialog box that stops the run.
json result_of(const json& reply) {
    const json& result = reply.at("result");
    const std::string text = result.at("content").at(0).at("text").get<std::string>();
    if (result.value("isError", false)) std::printf("      tool error: %s\n", text.c_str());
    CHECK(!result.value("isError", false));
    return json::parse(text);
}

} // namespace

// Four steps from reset run MOVEQ, ADDQ, MOVE and BRA: D0 is 1, the count 1 is
// stored at $30000, and the next instruction is the ADDQ at $102 -- `pc` is
// where the next instruction is, not the prefetch address. Reset puts it back
// at $100.
TEST(step_get_state_read_memory_and_reset) {
    ql::Engine engine(counting_rom(), 128);
    json state = result_of(call(engine, 1, "step", json{{"instructions", 4}}));
    CHECK_EQ(state.at("pc").get<std::string>(), std::string("000102"));
    CHECK_EQ(state.at("registers").at("d0").get<std::string>(), std::string("00000001"));
    CHECK_EQ(state.at("running").get<bool>(), false);

    state = result_of(call(engine, 2, "get_state"));
    CHECK_EQ(state.at("pc").get<std::string>(), std::string("000102"));
    CHECK_EQ(state.at("registers").at("ssp").get<std::string>(), std::string("00040000"));
    CHECK_EQ(state.at("registers").at("sr").get<std::string>(), std::string("2700"));

    json memory = result_of(call(engine, 3, "read_memory", json{{"address", "$30000"}, {"length", 4}}));
    CHECK_EQ(memory.at("address").get<std::string>(), std::string("030000"));
    CHECK_EQ(memory.at("hex").get<std::string>(), std::string("00000001"));
    memory = result_of(call(engine, 4, "read_memory", json{{"address", 0x30003}, {"length", 1}}));
    CHECK_EQ(memory.at("hex").get<std::string>(), std::string("01"));

    state = result_of(call(engine, 5, "reset"));
    CHECK_EQ(state.at("pc").get<std::string>(), std::string("000100"));
}

// run with wait:false returns at once and leaves the machine running; pause
// stops it, and the count it reached is in RAM.
TEST(run_without_waiting_then_pause) {
    ql::Engine engine(counting_rom(), 128);
    engine.set_uncapped(true);
    json running = result_of(call(engine, 1, "run", json{{"wait", false}}));
    CHECK_EQ(running.at("running").get<bool>(), true);
    CHECK_EQ(result_of(call(engine, 2, "get_state")).at("running").get<bool>(), true);

    call(engine, 3, "pause");
    bool stopped = false;
    for (int i = 0; i < 1000 && !stopped; i++) {
        stopped = !result_of(call(engine, 4, "get_state")).at("running").get<bool>();
    }
    CHECK(stopped);
    json memory = result_of(call(engine, 5, "read_memory", json{{"address", "$30000"}, {"length", 4}}));
    CHECK(memory.at("hex").get<std::string>() != std::string("00000000"));
}

// get_screen is a PNG of the 512x256 canvas: the PNG signature, then the IHDR
// chunk whose width and height are big-endian at bytes 16 and 20 (the PNG
// specification, 5.2 and 11.2.2). The pixels are the machine's own, tested
// there.
TEST(get_screen_is_a_512_by_256_png) {
    ql::Engine engine(counting_rom(), 128);
    json reply = call(engine, 1, "get_screen");
    const json& content = reply.at("result").at("content").at(0);
    CHECK_EQ(content.at("type").get<std::string>(), std::string("image"));
    CHECK_EQ(content.at("mimeType").get<std::string>(), std::string("image/png"));
    std::vector<uint8_t> png = ql::base64_decode(content.at("data").get<std::string>());
    CHECK(png.size() > 24);
    if (png.size() <= 24) return;
    const uint8_t signature[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    for (int i = 0; i < 8; i++) CHECK_EQ(png[i], signature[i]);
    CHECK(png[12] == 'I' && png[13] == 'H' && png[14] == 'D' && png[15] == 'R');
    auto be32 = [&png](size_t at) {
        return uint32_t(png[at]) << 24 | uint32_t(png[at + 1]) << 16 | uint32_t(png[at + 2]) << 8 | png[at + 3];
    };
    CHECK_EQ(be32(16), 512u);
    CHECK_EQ(be32(20), 256u);
}

// server_info names the server. An unknown tool is a tool error, an unknown
// method a JSON-RPC one, and a notification -- no id -- gets no reply at all.
TEST(server_info_and_errors) {
    ql::Engine engine(counting_rom(), 128);
    CHECK_EQ(result_of(call(engine, 1, "server_info")).at("name").get<std::string>(), std::string("ql_server"));

    json reply = call(engine, 2, "no_such_tool");
    CHECK_EQ(reply.at("result").at("isError").get<bool>(), true);
    reply = call(engine, 3, "read_memory", json{{"address", "nowhere"}});
    CHECK_EQ(reply.at("result").at("isError").get<bool>(), true);

    reply = send(engine, request(4, "no/such/method"));
    CHECK_EQ(reply.at("error").at("code").get<int>(), -32601);

    bool handled = true;
    ql::mcp::handle_message(engine, json{{"jsonrpc", "2.0"}, {"method", "notifications/initialized"}}, handled);
    CHECK(!handled);
}

RUN_TESTS()
