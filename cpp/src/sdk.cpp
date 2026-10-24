// SPDX-License-Identifier: MIT

#include "cerberus/sdk.hpp"

#if defined(CERBERUS_HAS_HTTP)
#include "mcp/http_client_transport.hpp"
#endif

#if !defined(_WIN32)
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;
#endif

#include <cstring>
#include <sstream>

namespace cerberus {

namespace {

std::string text_of(const std::vector<mcp::ContentBlock>& blocks) {
    std::string out;
    for (const auto& block : blocks) {
        if (auto* t = std::get_if<mcp::TextContent>(&block)) {
            if (!out.empty()) out += '\n';
            out += t->text;
        }
    }
    return out;
}

}  // namespace

// ---------------------------------------------------------------------------
// Error codes
// ---------------------------------------------------------------------------

std::string_view to_string(GatewayErrorCode code) noexcept {
    switch (code) {
        case GatewayErrorCode::UnknownTool:            return "unknown_tool";
        case GatewayErrorCode::ServerNotFound:         return "server_not_found";
        case GatewayErrorCode::PolicyDenied:           return "policy_denied";
        case GatewayErrorCode::ConfirmationRequired:   return "confirmation_required";
        case GatewayErrorCode::UpstreamUnavailable:    return "upstream_unavailable";
        case GatewayErrorCode::ShuttingDown:           return "shutting_down";
        case GatewayErrorCode::ConcurrencySaturated:   return "concurrency_saturated";
        case GatewayErrorCode::Timeout:                return "timeout";
        case GatewayErrorCode::UpstreamError:          return "upstream_error";
    }
    return "upstream_error";
}

GatewayErrorCode error_code_from_string(std::string_view code) noexcept {
    if (code == "unknown_tool")              return GatewayErrorCode::UnknownTool;
    if (code == "server_not_found")          return GatewayErrorCode::ServerNotFound;
    if (code == "policy_denied")             return GatewayErrorCode::PolicyDenied;
    if (code == "confirmation_required")     return GatewayErrorCode::ConfirmationRequired;
    if (code == "upstream_unavailable")      return GatewayErrorCode::UpstreamUnavailable;
    if (code == "shutting_down")             return GatewayErrorCode::ShuttingDown;
    if (code == "concurrency_saturated")     return GatewayErrorCode::ConcurrencySaturated;
    if (code == "timeout")                   return GatewayErrorCode::Timeout;
    if (code == "upstream_error")            return GatewayErrorCode::UpstreamError;
    return GatewayErrorCode::UpstreamError;
}

bool is_retryable(GatewayErrorCode code) noexcept {
    return code == GatewayErrorCode::UpstreamUnavailable ||
           code == GatewayErrorCode::ShuttingDown ||
           code == GatewayErrorCode::ConcurrencySaturated;
}

int exit_code(GatewayErrorCode code) noexcept {
    switch (code) {
        case GatewayErrorCode::UnknownTool:
        case GatewayErrorCode::ServerNotFound:         return 5;
        case GatewayErrorCode::PolicyDenied:
        case GatewayErrorCode::ConfirmationRequired:   return 6;
        case GatewayErrorCode::Timeout:
        case GatewayErrorCode::ConcurrencySaturated:   return 7;
        default:                                       return 1;
    }
}

GatewayError::GatewayError(GatewayErrorCode             code_in,
                           std::string                  message_in,
                           std::optional<bool>          retryable_in,
                           std::optional<std::string>   server_in,
                           std::optional<std::string>   tool_in,
                           std::optional<std::string>   reason_in)
    : std::runtime_error(std::string(to_string(code_in)) + ": " + message_in),
      code(code_in),
      message(std::move(message_in)),
      retryable(retryable_in.value_or(is_retryable(code_in))),
      exit_code(cerberus::exit_code(code_in)),
      server(std::move(server_in)),
      tool(std::move(tool_in)),
      reason(std::move(reason_in)) {}

// ---------------------------------------------------------------------------
// Tool routing
// ---------------------------------------------------------------------------

std::string public_tool_name(std::string_view server, std::string_view tool) {
    std::string out;
    out.reserve(server.size() + tool.size() + 2);
    out.append(server).append("__").append(tool);
    return out;
}

RoutedTool routed_tool(const mcp::Tool& tool) {
    // Preferred: the authoritative `_meta` route block.
    if (tool.meta && tool.meta->is_object()) {
        const auto it = tool.meta->find(std::string(kRouteMetaKey));
        if (it != tool.meta->end() && it->is_object()) {
            const auto s = it->find("server");
            const auto t = it->find("tool");
            if (s != it->end() && t != it->end() && s->is_string() && t->is_string()) {
                return RoutedTool{
                    .name        = tool.name,
                    .server      = s->get<std::string>(),
                    .tool        = t->get<std::string>(),
                    .description = tool.description,
                    .input_schema = tool.input_schema,
                };
            }
        }
    }
    // Fallback: first `__` in the public name (pre-contract servers).
    const std::size_t sep = tool.name.find("__");
    if (sep != std::string::npos && sep > 0 && sep + 2 < tool.name.size()) {
        return RoutedTool{
            .name        = tool.name,
            .server      = tool.name.substr(0, sep),
            .tool        = tool.name.substr(sep + 2),
            .description = tool.description,
            .input_schema = tool.input_schema,
        };
    }
    throw std::invalid_argument("tool `" + tool.name +
                                "` has no route metadata and no `__` separator");
}

// ---------------------------------------------------------------------------
// Error envelope
// ---------------------------------------------------------------------------

GatewayError gateway_error(const mcp::CallToolResult& result,
                           std::string_view            fallback_server,
                           std::string_view            fallback_tool) {
    const std::string text = text_of(result.content);
    const std::optional<std::string> server{fallback_server};
    const std::optional<std::string> tool{fallback_tool};

    if (!result.structured_content || !result.structured_content->is_object()) {
        return GatewayError(GatewayErrorCode::UpstreamError,
                            text.empty() ? std::string("tool call failed") : text,
                            std::nullopt, server, tool, std::nullopt);
    }
    const nlohmann::json& sc = *result.structured_content;

    GatewayErrorCode code = GatewayErrorCode::UpstreamError;
    if (const auto c = sc.find("code"); c != sc.end() && c->is_string()) {
        code = error_code_from_string(c->get_ref<const std::string&>());
    }

    std::string message;
    if (const auto m = sc.find("message"); m != sc.end() && m->is_string() && !m->get_ref<const std::string&>().empty()) {
        message = m->get<std::string>();
    } else {
        message = text.empty() ? std::string(to_string(code)) : text;
    }

    std::optional<bool> retryable;
    if (const auto r = sc.find("retryable"); r != sc.end() && r->is_boolean()) {
        retryable = r->get<bool>();
    }

    std::optional<std::string> sc_server = server;
    if (const auto s = sc.find("server"); s != sc.end() && s->is_string()) {
        sc_server = s->get<std::string>();
    }
    std::optional<std::string> sc_tool = tool;
    if (const auto t = sc.find("tool"); t != sc.end() && t->is_string()) {
        sc_tool = t->get<std::string>();
    }
    std::optional<std::string> reason;
    if (const auto rs = sc.find("reason"); rs != sc.end() && rs->is_string()) {
        reason = rs->get<std::string>();
    }

    return GatewayError(code, std::move(message), retryable,
                        std::move(sc_server), std::move(sc_tool),
                        std::move(reason));
}

// ---------------------------------------------------------------------------
// Gateway
// ---------------------------------------------------------------------------

#if !defined(_WIN32)
struct Gateway::Subprocess {
    pid_t pid     = -1;
    int   read_fd = -1;   // child stdout
    int   write_fd = -1;  // child stdin

    ~Subprocess() { reap(); }

    void reap() noexcept {
        if (write_fd >= 0) { ::close(write_fd); write_fd = -1; }
        if (read_fd  >= 0) { ::close(read_fd);  read_fd  = -1; }
        if (pid > 0) {
            ::kill(pid, SIGTERM);
            int status = 0;
            while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
            }
            pid = -1;
        }
    }
};
#else
struct Gateway::Subprocess {};
#endif

Gateway::Gateway(Gateway&&) noexcept = default;
Gateway& Gateway::operator=(Gateway&&) noexcept = default;

Gateway::~Gateway() {
    try {
        disconnect();
    } catch (...) {
    }
}

Gateway Gateway::connect_stdio() {
    return connect_stdio(StdioOptions{});
}

Gateway Gateway::connect_stdio(const StdioOptions& options) {
#if defined(_WIN32)
    (void)options;
    throw std::runtime_error(
        "cerberus-sdk stdio is POSIX-only; use Gateway::connect_http on Windows");
#else
    int in_pipe[2];   // parent write -> child stdin
    int out_pipe[2];  // child stdout -> parent read
    if (::pipe(in_pipe) != 0) {
        throw std::runtime_error("pipe() failed for gateway spawn");
    }
    if (::pipe(out_pipe) != 0) {
        ::close(in_pipe[0]);
        ::close(in_pipe[1]);
        throw std::runtime_error("pipe() failed for gateway spawn");
    }

    std::vector<std::string> argv;
    argv.push_back(options.binary);
    argv.push_back("gateway");
    argv.push_back("start");
    argv.push_back("--transport");
    argv.push_back("stdio");
    for (const auto& a : options.args) argv.push_back(a);

    posix_spawn_file_actions_t act;
    posix_spawn_file_actions_init(&act);
    posix_spawn_file_actions_adddup2(&act, in_pipe[0],  STDIN_FILENO);
    posix_spawn_file_actions_adddup2(&act, out_pipe[1], STDOUT_FILENO);
    posix_spawn_file_actions_addclose(&act, in_pipe[0]);
    posix_spawn_file_actions_addclose(&act, in_pipe[1]);
    posix_spawn_file_actions_addclose(&act, out_pipe[0]);
    posix_spawn_file_actions_addclose(&act, out_pipe[1]);

    std::vector<char*> cargv;
    cargv.reserve(argv.size() + 1);
    for (auto& a : argv) cargv.push_back(a.data());
    cargv.push_back(nullptr);

    pid_t pid = -1;
    const int rc = ::posix_spawnp(&pid, argv[0].c_str(), &act, nullptr,
                                  cargv.data(), environ);
    posix_spawn_file_actions_destroy(&act);
    ::close(in_pipe[0]);
    ::close(out_pipe[1]);
    if (rc != 0) {
        ::close(in_pipe[1]);
        ::close(out_pipe[0]);
        throw std::runtime_error("failed to spawn `" + options.binary +
                                 "`: " + std::string(std::strerror(rc)));
    }

    Gateway gw;
    gw.child_              = std::make_unique<Subprocess>();
    gw.child_->pid         = pid;
    gw.child_->write_fd    = in_pipe[1];
    gw.child_->read_fd     = out_pipe[0];

    mcp::StdioTransport::Options topts{};
    topts.read_fd  = gw.child_->read_fd;
    topts.write_fd = gw.child_->write_fd;
    auto transport  = std::make_unique<mcp::StdioTransport>(topts);

    gw.client_ = std::make_unique<mcp::Client>(mcp::Implementation{
        .name = "cerberus-sdk", .version = "0.1.0",
    });
    gw.client_->connect(std::move(transport));
    try {
        gw.client_->initialize().get();
    } catch (...) {
        gw.disconnect();
        throw;
    }
    gw.connected_ = true;
    return gw;
#endif
}

Gateway Gateway::connect_http(std::string_view                url,
                              std::optional<std::string> bearer) {
#if defined(CERBERUS_HAS_HTTP)
    mcp::HttpClientTransport::Options opts;
    opts.url = std::string(url);
    if (bearer) opts.access_token = std::move(*bearer);

    Gateway gw;
    gw.client_ = std::make_unique<mcp::Client>(mcp::Implementation{
        .name = "cerberus-sdk", .version = "0.1.0",
    });
    gw.client_->connect(std::make_unique<mcp::HttpClientTransport>(std::move(opts)));
    try {
        gw.client_->initialize().get();
    } catch (...) {
        gw.disconnect();
        throw;
    }
    gw.connected_ = true;
    return gw;
#else
    (void)url;
    (void)bearer;
    throw std::runtime_error("cerberus-sdk built without HTTP support");
#endif
}

void Gateway::disconnect() {
    if (client_ && connected_) {
        client_->disconnect();
        connected_ = false;
    }
    if (child_) {
        child_->reap();
        child_.reset();
    }
}

std::vector<RoutedTool> Gateway::list_tools() {
    auto result = client_->list_tools().get();
    std::vector<RoutedTool> out;
    out.reserve(result.tools.size());
    for (const auto& t : result.tools) out.push_back(routed_tool(t));
    return out;
}

ToolOutput Gateway::call(std::string_view      server,
                         std::string_view      tool,
                         const nlohmann::json& args) {
    auto result = client_->call_tool(public_tool_name(server, tool), args).get();
    if (result.is_error.value_or(false)) {
        throw gateway_error(result, server, tool);
    }
    return ToolOutput{
        .text       = text_of(result.content),
        .structured = result.structured_content,
    };
}

}  // namespace cerberus
