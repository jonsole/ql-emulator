#include "mcp_server.h"

#include "base64.h"
#include "http.h"
#include "log.h"
#include "png.h"

#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

namespace ql::mcp {

using json = nlohmann::json;

namespace {

/// The MCP revision spoken, zx_server's.
constexpr const char* PROTOCOL_VERSION = "2025-06-18";
constexpr const char* SERVER_NAME = "ql_server";
constexpr const char* SERVER_VERSION = "0.1.0";

constexpr const char* SERVER_INSTRUCTIONS =
    "A Sinclair QL emulator: a 68008 at 7.5 MHz with the QL's memory map and screen. "
    "`run` with wait:false starts the machine and returns; `pause` stops it. `step` runs whole "
    "instructions; `get_state` gives the registers and where execution is; `read_memory` reads "
    "bytes; `get_screen` returns the 512x256 screen as a PNG. Addresses are hex, six digits: "
    "pass them as numbers or as strings such as \"$30000\".";

// JSON-RPC error codes.
constexpr int PARSE_ERROR = -32700;
constexpr int INVALID_REQUEST = -32600;
constexpr int METHOD_NOT_FOUND = -32601;
constexpr int INVALID_PARAMS = -32602;

json rpc_error(const json& id, int code, const std::string& message) {
    return json{{"jsonrpc", "2.0"}, {"id", id}, {"error", json{{"code", code}, {"message", message}}}};
}

json rpc_result(const json& id, const json& result) {
    return json{{"jsonrpc", "2.0"}, {"id", id}, {"result", result}};
}

json tool(const char* name, const char* description, json properties = json::object()) {
    return json{{"name", name},
                {"description", description},
                {"inputSchema", json{{"type", "object"}, {"properties", std::move(properties)}}}};
}

json tools_list() {
    json address = json{{"type", json::array({"integer", "string"})},
                        {"description", "an address: a number, or hex such as \"$30000\""}};
    return json::array({
        tool("run", "Runs the machine. With wait:false, returns at once and leaves it running; "
                    "otherwise returns when paused.",
             json{{"wait", json{{"type", "boolean"}}}}),
        tool("pause", "Stops a run."),
        tool("step", "Runs whole instructions (default 1).",
             json{{"instructions", json{{"type", "integer"}, {"minimum", 1}}}}),
        tool("reset", "The reset button: the 68008's reset sequence. RAM is left as it was."),
        tool("get_state", "The registers, where execution is, and whether the machine is running."),
        tool("read_memory", "Reads bytes as the CPU would, returned as hex.",
             json{{"address", address}, {"length", json{{"type", "integer"}, {"minimum", 0}}}}),
        tool("get_screen", "The screen as a 512x256 PNG."),
        tool("server_info", "The server's name and version."),
    });
}

// ---- tool results, as zx_server's ------------------------------------------

json text_result(const std::string& message) {
    return json{{"content", json::array({json{{"type", "text"}, {"text", message}}})}};
}

json json_result(const json& value) {
    return text_result(value.dump());
}

/// A tool failure: per the MCP spec a successful JSON-RPC response carrying
/// isError, so the model sees the message and can adapt.
json error_result(const std::string& message) {
    json result = text_result(message);
    result["isError"] = true;
    return result;
}

// ---- formatting and arguments -----------------------------------------------

std::string hex(uint32_t value, int digits) {
    char text[16];
    std::snprintf(text, sizeof text, "%0*X", digits, value);
    return text;
}

/// Addresses are six hex digits (docs/plan.md).
std::string address_text(uint32_t address) {
    return hex(address & 0xFFFFFF, 6);
}

json state_json(const MachineState& s, bool running) {
    const Registers& r = s.registers;
    json registers = json::object();
    for (int i = 0; i < 8; i++) registers["d" + std::to_string(i)] = hex(r.d[i], 8);
    for (int i = 0; i < 7; i++) registers["a" + std::to_string(i)] = hex(r.a[i], 8);
    // A7 is whichever stack pointer the S bit selects.
    registers["a7"] = hex((r.sr & 0x2000) ? r.ssp : r.usp, 8);
    registers["usp"] = hex(r.usp, 8);
    registers["ssp"] = hex(r.ssp, 8);
    registers["sr"] = hex(r.sr, 4);
    // The next instruction is the one in IR, 4 behind the prefetch address
    // the core keeps as PC (Registers::pc).
    return json{{"pc", address_text(r.pc - 4)},
                {"running", running},
                {"stopped", s.stopped},
                {"clock", s.clock},
                {"registers", registers}};
}

const json& arg(const json& args, const char* name) {
    static const json missing;
    auto found = args.find(name);
    return found == args.end() ? missing : *found;
}

/// An address argument: a number, or hex with an optional $ or 0x. False with
/// `error` set if it is neither.
bool parse_address(const json& value, uint32_t& address, std::string& error) {
    if (value.is_number_integer() && value.get<int64_t>() >= 0 && value.get<int64_t>() <= 0xFFFFFFFF) {
        address = static_cast<uint32_t>(value.get<int64_t>());
        return true;
    }
    if (value.is_string()) {
        std::string text = value.get<std::string>();
        if (!text.empty() && text[0] == '$') text = text.substr(1);
        else if (text.size() > 1 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) text = text.substr(2);
        if (!text.empty() && text.size() <= 8
            && text.find_first_not_of("0123456789abcdefABCDEF") == std::string::npos) {
            address = static_cast<uint32_t>(std::stoul(text, nullptr, 16));
            return true;
        }
    }
    error = "'address' must be a number or hex, such as \"$30000\"";
    return false;
}

/// The largest read_memory serves at once.
constexpr int64_t MAX_READ = 0x10000;

json image_result(const std::vector<uint8_t>& png) {
    return json{{"content", json::array({json{{"type", "image"}, {"data", base64_encode(png)}, {"mimeType", "image/png"}}})}};
}

json call_tool(Engine& engine, const std::string& name, const json& args) {
    if (name == "run") {
        // A run is a queued job, so a second would sit behind the first and
        // start the moment a pause ended it.
        if (engine.running()) return error_result("already running -- pause it first");
        const json& wait = arg(args, "wait");
        if (wait.is_boolean() && !wait.get<bool>()) {
            std::thread([&engine] { engine.run(); }).detach();
            // The run marks itself running on its own thread; a moment for it
            // to, so a get_state straight after sees a machine that has
            // started (zx_server's arrangement).
            for (int i = 0; i < 250 && !engine.running(); i++) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            return json_result(state_json(engine.state(), engine.running()));
        }
        // Waits for a pause, which bypasses the queue and so can still reach
        // it.
        return json_result(state_json(engine.run(), false));
    }
    if (name == "pause") {
        engine.pause();
        return text_result("paused");
    }
    if (name == "get_screen") {
        return image_result(encode_png(engine.screen()));
    }
    if (name == "server_info") {
        return json_result(json{{"name", SERVER_NAME}, {"version", SERVER_VERSION}});
    }
    if (name == "step") {
        const json& count = arg(args, "instructions");
        int64_t instructions = count.is_number_integer() ? count.get<int64_t>() : 1;
        if (instructions < 1) return error_result("'instructions' must be at least 1");
        if (engine.running()) return error_result("running -- pause it first");
        return json_result(state_json(engine.step(uint32_t(instructions)), false));
    }
    if (name == "reset") {
        if (engine.running()) return error_result("running -- pause it first");
        return json_result(state_json(engine.reset(), false));
    }
    if (name == "get_state") {
        return json_result(state_json(engine.state(), engine.running()));
    }
    if (name == "read_memory") {
        uint32_t address = 0;
        std::string error;
        if (!parse_address(arg(args, "address"), address, error)) return error_result(error);
        const json& len = arg(args, "length");
        int64_t length = len.is_number_integer() ? len.get<int64_t>() : 1;
        if (length < 0 || length > MAX_READ) return error_result("'length' must be between 0 and 65536");
        std::vector<uint8_t> bytes = engine.read_memory(address, uint32_t(length));
        std::string text;
        for (uint8_t b : bytes) text += hex(b, 2);
        return json_result(json{{"address", address_text(address)}, {"hex", text}});
    }
    return error_result("unknown tool: " + name);
}

} // namespace

json handle_message(Engine& engine, const json& message, bool& handled) {
    handled = true;
    const json id = message.contains("id") ? message["id"] : json();
    const std::string method = message.value("method", std::string());
    if (method.empty()) return rpc_error(id, INVALID_REQUEST, "missing 'method'");
    // Notifications carry no id and must not be answered.
    if (!message.contains("id")) {
        handled = false;
        return json();
    }
    if (method == "initialize") {
        log("MCP  client connected");
        return rpc_result(id, json{{"protocolVersion", PROTOCOL_VERSION},
                                   {"capabilities", json{{"tools", json{{"listChanged", false}}}}},
                                   {"serverInfo", json{{"name", SERVER_NAME}, {"version", SERVER_VERSION}}},
                                   {"instructions", SERVER_INSTRUCTIONS}});
    }
    if (method == "ping") return rpc_result(id, json::object());
    if (method == "tools/list") return rpc_result(id, json{{"tools", tools_list()}});
    if (method == "tools/call") {
        static const json empty = json::object();
        const json& params = message.contains("params") && message["params"].is_object() ? message["params"] : empty;
        const json& name = arg(params, "name");
        if (!name.is_string()) return rpc_error(id, INVALID_PARAMS, "missing tool 'name'");
        const json& arguments = arg(params, "arguments").is_object() ? arg(params, "arguments") : empty;
        // Every tool call is logged: the only record of what an agent did to
        // the machine, since nobody watched it happen.
        log("MCP  %s", name.get<std::string>().c_str());
        return rpc_result(id, call_tool(engine, name.get<std::string>(), arguments));
    }
    return rpc_error(id, METHOD_NOT_FOUND, "unknown method: " + method);
}

namespace {

// ---- HTTP, as zx_server's --------------------------------------------------

void handle_post(Engine& engine, const http::Request& request, http::Response& response) {
    const json message = json::parse(request.body, nullptr, /*allow_exceptions=*/false);
    if (message.is_discarded()) {
        response.status = 400;
        response.body = rpc_error(json(), PARSE_ERROR, "invalid JSON").dump();
        return;
    }
    // A client may batch several messages into one array.
    if (message.is_array()) {
        json replies = json::array();
        for (const json& one : message) {
            bool handled = false;
            json reply = handle_message(engine, one, handled);
            if (handled) replies.push_back(std::move(reply));
        }
        if (replies.empty()) {
            response.status = 202;   // nothing but notifications
            return;
        }
        response.body = replies.dump();
        return;
    }
    bool handled = false;
    json reply = handle_message(engine, message, handled);
    if (!handled) {
        response.status = 202;   // a notification: accepted, nothing to say back
        return;
    }
    response.body = reply.dump();
}

void handle_connection(net::Socket sock, Engine& engine) {
    std::string buffer;
    for (;;) {
        http::Request request;
        if (!http::read_request(sock, buffer, request)) return;   // EOF, or unparseable
        http::Response response;
        if (request.target != "/mcp") {
            response.status = 404;
            response.body = R"({"error":"not found; MCP is served at /mcp"})";
        } else if (request.method == "POST") {
            handle_post(engine, request, response);
        } else if (request.method == "DELETE") {
            // Session teardown. Nothing is kept per session -- there is one
            // machine, shared -- so this is only an acknowledgement.
            log("MCP  client disconnected");
            response.status = 202;
        } else {
            // GET included: this server never starts a message, so it has no
            // event stream to offer, which the transport allows it to refuse.
            response.status = 405;
            response.body = R"({"error":"method not allowed"})";
        }
        if (!http::write_response(sock, response)) return;
    }
}

} // namespace

void serve(Engine& engine, net::Listener& listener) {
    for (;;) {
        net::Socket sock = listener.accept();
        if (!sock.valid()) {
            if (!listener.valid()) return;   // closed: stop serving
            continue;
        }
        std::thread([s = std::move(sock), &engine]() mutable { handle_connection(std::move(s), engine); }).detach();
    }
}

} // namespace ql::mcp
