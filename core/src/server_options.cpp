#include "server_options.h"

namespace ql {

namespace {

/// A whole decimal number, and nothing else.
bool parse_number(const std::string& text, unsigned long& value) {
    if (text.empty() || text.find_first_not_of("0123456789") != std::string::npos || text.size() > 9) return false;
    value = std::stoul(text);
    return true;
}

} // namespace

bool parse_server_options(const std::vector<std::string>& args, ServerOptions& options, std::string& error) {
    for (size_t i = 0; i < args.size(); i++) {
        const std::string& arg = args[i];
        if (arg == "--uncapped") {
            options.uncapped = true;
            continue;
        }
        if (arg != "--rom" && arg != "--ram" && arg != "--mcp-host" && arg != "--mcp-port") {
            error = "unknown option: " + arg;
            return false;
        }
        if (i + 1 >= args.size()) {
            error = arg + " needs a value";
            return false;
        }
        const std::string& value = args[++i];
        if (arg == "--rom") {
            options.rom = value;
        } else if (arg == "--ram") {
            unsigned long kb = 0;
            if (!parse_number(value, kb) || (kb != 128 && kb != 640)) {
                error = "--ram must be 128 or 640";
                return false;
            }
            options.ram_kb = unsigned(kb);
        } else if (arg == "--mcp-host") {
            options.mcp_host = value;
        } else {
            unsigned long port = 0;
            if (!parse_number(value, port) || port > 65535) {
                error = "--mcp-port must be a port number, 0-65535";
                return false;
            }
            options.mcp_port = uint16_t(port);
        }
    }
    if (options.rom.empty()) {
        error = "--rom is required: the system ROM image (the JS ROM, or Minerva)";
        return false;
    }
    return true;
}

std::string server_usage() {
    return "usage: ql_server --rom <image> [--ram 128|640] [--mcp-host <host>] [--mcp-port <port>] [--uncapped]\n"
           "\n"
           "  --rom <image>       the system ROM, up to 48K (required)\n"
           "  --ram 128|640       RAM in K: the standard 128K, or 640K with the expansion (default 128)\n"
           "  --mcp-host <host>   the address MCP listens on (default 127.0.0.1)\n"
           "  --mcp-port <port>   MCP's port (default 8100)\n"
           "  --uncapped          run as fast as possible rather than in realtime\n";
}

} // namespace ql
