#pragma once
// ql_server's command line (docs/plan.md, The server; docs/server.md).

#include <cstdint>
#include <string>
#include <vector>

namespace ql {

struct ServerOptions {
    std::string rom;                  ///< the system ROM image; required
    unsigned ram_kb = 128;            ///< 128, or 640 with the expansion
    std::string mcp_host = "127.0.0.1";
    uint16_t mcp_port = 8100;         ///< zx_server's 8000 moved, so both run side by side
    bool uncapped = false;            ///< as fast as it goes, not realtime
};

/// Parses the arguments after the program name. False, with `error` saying
/// why, on anything it does not understand; nothing is guessed at.
bool parse_server_options(const std::vector<std::string>& args, ServerOptions& options, std::string& error);

/// What `--help` prints.
std::string server_usage();

} // namespace ql
