// SPDX-License-Identifier: MIT
//
// C++ SDK for the CerberusMCP gateway, wrapping the community mcp-cpp
// client (no official C++ MCP SDK exists).
//
// Two Cerberus additions on top of plain MCP:
//   - RoutedTool: every listed tool resolves its upstream server/tool from
//     `_meta["cerberus.dev/route"]` — no `{server}__{tool}` splitting.
//   - GatewayError: failing calls throw a typed error carrying the gateway
//     `code`, `retryable`, and the CLI-aligned `exit_code`.
//
// Wire contract: ../docs/contract.md
#pragma once

#include "mcp/mcp.hpp"

#include <nlohmann/json.hpp>

#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#if !defined(_WIN32)
#include <sys/types.h>
#endif

namespace cerberus {

/// Reverse-DNS `_meta` key carrying `{server, tool}` on every listed tool.
inline constexpr std::string_view kRouteMetaKey = "cerberus.dev/route";

/// Stable gateway error codes (snake_case on the wire).
enum class GatewayErrorCode {
    UnknownTool,
    ServerNotFound,
    PolicyDenied,
    ConfirmationRequired,
    UpstreamUnavailable,
    ShuttingDown,
    ConcurrencySaturated,
    Timeout,
    UpstreamError,
};

/// Wire spelling of a code (`policy_denied`, ...).
std::string_view to_string(GatewayErrorCode code) noexcept;

/// Parse a wire code; unknown spellings map to `GatewayErrorCode::UpstreamError`.
GatewayErrorCode error_code_from_string(std::string_view code) noexcept;

/// Whether an SDK may automatically retry a call that failed this way.
bool is_retryable(GatewayErrorCode code) noexcept;

/// Process exit code the `cerberus` CLI reports for the same condition.
int exit_code(GatewayErrorCode code) noexcept;

/// A gateway-reported tool failure (from an `isError: true` result).
struct GatewayError : std::runtime_error {
    GatewayError(GatewayErrorCode code_in,
                 std::string     message_in,
                 std::optional<bool> retryable_in,
                 std::optional<std::string> server_in,
                 std::optional<std::string> tool_in,
                 std::optional<std::string> reason_in);

    GatewayErrorCode code;
    std::string      message;
    bool             retryable;
    int              exit_code;
    std::optional<std::string> server;
    std::optional<std::string> tool;
    std::optional<std::string> reason;
};

/// A tool as listed by the gateway, with upstream coordinates resolved.
struct RoutedTool {
    std::string                   name;   ///< public name (`{server}__{tool}`)
    std::string                   server;
    std::string                   tool;   ///< the upstream's own tool name
    std::optional<std::string>    description;
    nlohmann::json                input_schema = nlohmann::json::object();
};

/// Public name for an upstream tool.
std::string public_tool_name(std::string_view server, std::string_view tool);

/// Resolve a listed tool's upstream coordinates. Prefers
/// `_meta["cerberus.dev/route"]`, falls back to first-`__` name parsing.
/// Throws `std::invalid_argument` when neither is possible.
RoutedTool routed_tool(const mcp::Tool& tool);

/// A successful tool result: structured payload when present, text always.
struct ToolOutput {
    std::string                  text;
    std::optional<nlohmann::json> structured;
};

/// Build a `GatewayError` from an `isError: true` result. Prefers
/// `structuredContent`; degrades to `upstream_error` without it.
GatewayError gateway_error(const mcp::CallToolResult& result,
                           std::string_view            fallback_server,
                           std::string_view            fallback_tool);

/// A connection to a Cerberus gateway (stdio, or streamable HTTP when built
/// with `CERBERUS_ENABLE_HTTP=ON`).
///
/// The stdio transport is POSIX-only (mcp-cpp limitation): it wires an
/// explicitly spawned `cerberus gateway start --transport stdio` child.
class Gateway {
public:
    struct StdioOptions {
        /// Binary to spawn (resolved through `PATH` by `posix_spawn`).
        std::string              binary = "cerberus";
        /// Extra CLI args appended after `gateway start --transport stdio`.
        std::vector<std::string> args;
    };

    /// Spawn the gateway and complete the MCP handshake. Throws
    /// `std::runtime_error` when the process cannot be spawned and
    /// `mcp::Error` when the handshake fails.
    static Gateway connect_stdio();
    static Gateway connect_stdio(const StdioOptions& options);

    /// Connect to a streamable-HTTP gateway endpoint. Throws
    /// `std::runtime_error("cerberus-sdk built without HTTP support")`
    /// when compiled with `CERBERUS_ENABLE_HTTP=OFF`.
    static Gateway connect_http(std::string_view url,
                                std::optional<std::string> bearer = std::nullopt);

    Gateway(const Gateway&) = delete;
    Gateway& operator=(const Gateway&) = delete;
    Gateway(Gateway&&) noexcept;
    Gateway& operator=(Gateway&&) noexcept;
    ~Gateway();

    /// The underlying mcp-cpp client, for APIs this SDK does not wrap.
    const mcp::Client& client() const noexcept { return *client_; }

    /// `tools/list` with upstream coordinates resolved from `_meta`.
    std::vector<RoutedTool> list_tools();

    /// `tools/call` for `server::tool`. Throws `GatewayError` when the
    /// gateway reports `isError: true`; other failures surface as
    /// `mcp::Error` from the transport/protocol layer.
    ToolOutput call(std::string_view      server,
                    std::string_view      tool,
                    const nlohmann::json& args = nlohmann::json::object());

    /// Tear down the session and reap the spawned gateway (if any).
    void disconnect();

private:
    Gateway() = default;

    struct Subprocess;

    std::unique_ptr<mcp::Client>  client_;
    std::unique_ptr<Subprocess>   child_;
    bool                          connected_ = false;
};

}  // namespace cerberus
