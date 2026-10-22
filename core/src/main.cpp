// ql_server: the QL emulator, driven over MCP (docs/server.md).
//
// Everything is bound before anything is served, and any failure stops the
// server with a message, as zx_server does: a server half up, with one port
// taken by something else, is worse than one that says so and exits.

#include "engine.h"
#include "file_io.h"
#include "log.h"
#include "mcp_server.h"
#include "net.h"
#include "server_options.h"

#include <cstdio>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

int main(int argc, char** argv) {
    std::vector<std::string> args(argv + 1, argv + argc);
    for (const std::string& arg : args) {
        if (arg == "--help" || arg == "-h") {
            std::fputs(ql::server_usage().c_str(), stdout);
            return 0;
        }
    }
    ql::ServerOptions options;
    std::string error;
    if (!ql::parse_server_options(args, options, error)) {
        std::fprintf(stderr, "ql_server: %s\n\n%s", error.c_str(), ql::server_usage().c_str());
        return 2;
    }

    std::vector<uint8_t> rom;
    if (!ql::read_file(options.rom, rom)) {
        std::fprintf(stderr, "ql_server: cannot read the ROM %s\n", options.rom.c_str());
        return 1;
    }
    std::unique_ptr<ql::Engine> engine;
    try {
        engine = std::make_unique<ql::Engine>(std::move(rom), options.ram_kb);
    } catch (const std::invalid_argument& e) {
        std::fprintf(stderr, "ql_server: %s: %s\n", options.rom.c_str(), e.what());
        return 1;
    }
    engine->set_uncapped(options.uncapped);

    ql::net::Listener mcp_listener;
    if (!mcp_listener.listen(options.mcp_host, options.mcp_port, error)) {
        std::fprintf(stderr, "ql_server: MCP: %s\n", error.c_str());
        return 1;
    }
    ql::log("ql_server: %s, %uK, %s", options.rom.c_str(), options.ram_kb,
            options.uncapped ? "uncapped" : "realtime");
    ql::log("MCP  http://%s:%u/mcp", options.mcp_host.c_str(), unsigned(mcp_listener.port()));
    ql::mcp::serve(*engine, mcp_listener);
    return 0;
}
