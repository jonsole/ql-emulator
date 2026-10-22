// ql_server's command line: --rom, --ram 128|640, --mcp-host, --mcp-port
// and --uncapped (docs/plan.md, The server), with the defaults the plan
// gives: 128K, and MCP on 127.0.0.1:8100.

#include "server_options.h"
#include "test_main.h"

#include <string>
#include <vector>

namespace {

ql::ServerOptions parse(std::vector<std::string> args, std::string& error) {
    ql::ServerOptions options;
    error.clear();
    if (!ql::parse_server_options(args, options, error) && error.empty()) error = "(no message)";
    return options;
}

} // namespace

TEST(defaults_and_every_option) {
    std::string error;
    ql::ServerOptions o = parse({"--rom", "roms/js.rom"}, error);
    CHECK_EQ(error, std::string());
    CHECK_EQ(o.rom, std::string("roms/js.rom"));
    CHECK_EQ(o.ram_kb, 128u);
    CHECK_EQ(o.mcp_host, std::string("127.0.0.1"));
    CHECK_EQ(o.mcp_port, uint16_t{8100});
    CHECK(!o.uncapped);

    o = parse({"--rom", "x.rom", "--ram", "640", "--mcp-host", "0.0.0.0", "--mcp-port", "18100", "--uncapped"},
              error);
    CHECK_EQ(error, std::string());
    CHECK_EQ(o.ram_kb, 640u);
    CHECK_EQ(o.mcp_host, std::string("0.0.0.0"));
    CHECK_EQ(o.mcp_port, uint16_t{18100});
    CHECK(o.uncapped);
}

// Each mistake is refused with a message, never guessed at.
TEST(mistakes_are_refused) {
    const std::vector<std::vector<std::string>> mistakes = {
        {},                                       // no --rom
        {"--rom"},                                // --rom without a value
        {"--rom", "x.rom", "--ram", "256"},       // not 128 or 640
        {"--rom", "x.rom", "--mcp-port", "70000"},
        {"--rom", "x.rom", "--mcp-port", "port"},
        {"--rom", "x.rom", "--frobnicate"},
    };
    for (const auto& args : mistakes) {
        std::string error;
        parse(args, error);
        CHECK(!error.empty());
    }
}

RUN_TESTS()
