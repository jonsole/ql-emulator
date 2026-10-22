#pragma once
// MCP: the Model Context Protocol server, JSON-RPC 2.0 over Streamable HTTP,
// through which tools drive the machine.
//
// Cut down from zx_server's to what Stage 2 needs. Tool names keep
// zx_server's wherever the meaning is the same (docs/plan.md); docs/server.md
// lists them.

#include "engine.h"
#include "net.h"

#include <nlohmann/json.hpp>

namespace ql::mcp {

/// Handles one JSON-RPC message and returns the reply. `handled` comes back
/// false for a notification, which by definition gets no reply.
nlohmann::json handle_message(Engine& engine, const nlohmann::json& message, bool& handled);

/// Serves MCP at /mcp on connections to `listener`, a thread each, until the
/// listener is closed.
void serve(Engine& engine, net::Listener& listener);

} // namespace ql::mcp
