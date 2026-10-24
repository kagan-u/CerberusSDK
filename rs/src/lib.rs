//! Rust SDK for the CerberusMCP gateway.
//!
//! Wraps the official MCP client ([`rmcp`]) with Cerberus-specific
//! ergonomics:
//!
//! - [`RoutedTool`]: every listed tool carries its upstream `server`/`tool`
//!   from `_meta["cerberus.dev/route"]`, so callers never split the
//!   `{server}__{tool}` public name.
//! - [`GatewayError`]: failing calls map to a typed [`GatewayErrorCode`]
//!   with `retryable`/`exit_code`, instead of prose in `content`.
//! - [`Gateway`]: connects over stdio (spawning `cerberus gateway start`) or
//!   streamable HTTP.
//!
//! The wire contract lives in `docs/contract.md`.

use rmcp::model::{CallToolRequestParams, CallToolResult, ContentBlock, JsonObject, Tool};
use rmcp::service::RunningService;
use rmcp::transport::streamable_http_client::StreamableHttpClientTransportConfig;
use rmcp::transport::{ConfigureCommandExt, StreamableHttpClientTransport, TokioChildProcess};
use rmcp::{RoleClient, ServiceExt};
use serde::{Deserialize, Serialize};
use serde_json::Value;
use std::time::Duration;

/// Reverse-DNS `_meta` key carrying `{server, tool}` on every listed tool.
pub const ROUTE_META_KEY: &str = "cerberus.dev/route";

const DEFAULT_CONNECT_TIMEOUT: Duration = Duration::from_secs(30);

// ---------------------------------------------------------------------------
// Errors
// ---------------------------------------------------------------------------

/// Local failures (no gateway-shaped error payload was received).
#[derive(Debug, thiserror::Error)]
pub enum Error {
    #[error("io: {0}")]
    Io(#[from] std::io::Error),
    #[error("connect failed: {0}")]
    Connect(String),
    #[error("protocol: {0}")]
    Protocol(String),
    #[error("json: {0}")]
    Json(#[from] serde_json::Error),
}

/// Stable gateway error codes (snake_case on the wire, per `docs/contract.md`).
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum GatewayErrorCode {
    UnknownTool,
    ServerNotFound,
    PolicyDenied,
    ConfirmationRequired,
    UpstreamUnavailable,
    ShuttingDown,
    ConcurrencySaturated,
    Timeout,
    UpstreamError,
}

impl GatewayErrorCode {
    pub fn as_str(self) -> &'static str {
        match self {
            Self::UnknownTool => "unknown_tool",
            Self::ServerNotFound => "server_not_found",
            Self::PolicyDenied => "policy_denied",
            Self::ConfirmationRequired => "confirmation_required",
            Self::UpstreamUnavailable => "upstream_unavailable",
            Self::ShuttingDown => "shutting_down",
            Self::ConcurrencySaturated => "concurrency_saturated",
            Self::Timeout => "timeout",
            Self::UpstreamError => "upstream_error",
        }
    }

    /// Whether an SDK may automatically retry a call that failed this way.
    pub fn retryable(self) -> bool {
        matches!(
            self,
            Self::UpstreamUnavailable | Self::ShuttingDown | Self::ConcurrencySaturated
        )
    }

    /// Process exit code the `cerberus` CLI reports for the same condition.
    pub fn exit_code(self) -> u8 {
        match self {
            Self::UnknownTool | Self::ServerNotFound => 5,
            Self::PolicyDenied | Self::ConfirmationRequired => 6,
            Self::Timeout | Self::ConcurrencySaturated => 7,
            Self::UpstreamUnavailable | Self::ShuttingDown | Self::UpstreamError => 1,
        }
    }
}

impl std::fmt::Display for GatewayErrorCode {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.write_str(self.as_str())
    }
}

impl std::str::FromStr for GatewayErrorCode {
    type Err = Error;
    fn from_str(s: &str) -> Result<Self, Self::Err> {
        serde_json::from_value(Value::String(s.to_string()))
            .map_err(|_| Error::Protocol(format!("unknown gateway error code `{s}`")))
    }
}

/// A gateway-reported tool failure (`structuredContent` on `isError: true`).
#[derive(Debug, Clone, PartialEq)]
pub struct ToolError {
    pub code: GatewayErrorCode,
    pub message: String,
    pub retryable: bool,
    pub server: Option<String>,
    pub tool: Option<String>,
    pub reason: Option<String>,
}

/// Failure of a `tools/call`: either a gateway-shaped [`ToolError`], or a
/// local transport/protocol problem ([`Error`]).
#[derive(Debug, thiserror::Error)]
pub enum GatewayError {
    #[error("{code}: {message}")]
    Tool {
        code: GatewayErrorCode,
        message: String,
        retryable: bool,
        server: Option<String>,
        tool: Option<String>,
        reason: Option<String>,
    },
    #[error(transparent)]
    Local(#[from] Error),
}

impl GatewayError {
    pub fn as_tool_error(&self) -> Option<&ToolError> {
        match self {
            GatewayError::Tool { .. } => None, // plain accessors below
            GatewayError::Local(_) => None,
        }
    }

    pub fn code(&self) -> Option<GatewayErrorCode> {
        match self {
            GatewayError::Tool { code, .. } => Some(*code),
            GatewayError::Local(_) => None,
        }
    }

    pub fn retryable(&self) -> bool {
        match self {
            GatewayError::Tool { retryable, .. } => *retryable,
            GatewayError::Local(_) => false,
        }
    }
}

// ---------------------------------------------------------------------------
// Tool routing
// ---------------------------------------------------------------------------

/// A tool as listed by the gateway, with its upstream coordinates resolved.
#[derive(Debug, Clone, PartialEq)]
pub struct RoutedTool {
    /// The public name to pass to `tools/call` (`{server}__{tool}`).
    pub name: String,
    pub server: String,
    /// The tool's own name on its upstream server.
    pub tool: String,
    pub description: Option<String>,
    pub input_schema: Value,
}

/// Public name for an upstream tool.
pub fn public_tool_name(server: &str, tool: &str) -> String {
    format!("{server}__{tool}")
}

fn route_from_meta(meta: &rmcp::model::MetaObject) -> Option<(String, String)> {
    let route = meta.0.get(ROUTE_META_KEY)?;
    let server = route.get("server")?.as_str()?.to_string();
    let tool = route.get("tool")?.as_str()?.to_string();
    Some((server, tool))
}

fn route_from_name(name: &str) -> Option<(String, String)> {
    let (server, tool) = name.split_once("__")?;
    if server.is_empty() || tool.is_empty() {
        return None;
    }
    Some((server.to_string(), tool.to_string()))
}

impl TryFrom<Tool> for RoutedTool {
    type Error = Error;
    /// Prefers `_meta["cerberus.dev/route"]`; falls back to first-`__`
    /// name parsing for pre-contract servers.
    fn try_from(tool: Tool) -> Result<Self, Self::Error> {
        let name = tool.name.to_string();
        let route = tool
            .meta
            .as_ref()
            .and_then(route_from_meta)
            .or_else(|| route_from_name(&name))
            .ok_or_else(|| {
                Error::Protocol(format!(
                    "tool `{name}` has no route metadata and no `__` separator"
                ))
            })?;
        let input = serde_json::to_value(&*tool.input_schema)?;
        Ok(Self {
            name,
            server: route.0,
            tool: route.1,
            description: tool.description.as_ref().map(|d| d.to_string()),
            input_schema: input,
        })
    }
}

// ---------------------------------------------------------------------------
// Error envelope parsing
// ---------------------------------------------------------------------------

fn text_of(result: &CallToolResult) -> String {
    result
        .content
        .iter()
        .filter_map(|b| match b {
            ContentBlock::Text(t) => Some(t.text.as_str()),
            _ => None,
        })
        .collect::<Vec<_>>()
        .join("\n")
}

/// Builds a [`GatewayError::Tool`] from an `isError: true` result, preferring
/// `structuredContent` and degrading to `upstream_error` without it.
pub fn gateway_error(
    result: CallToolResult,
    fallback_server: &str,
    fallback_tool: &str,
) -> GatewayError {
    let text = text_of(&result);
    let Some(sc) = result.structured_content else {
        return GatewayError::Tool {
            code: GatewayErrorCode::UpstreamError,
            message: if text.is_empty() {
                "tool call failed".into()
            } else {
                text
            },
            retryable: GatewayErrorCode::UpstreamError.retryable(),
            server: Some(fallback_server.to_string()),
            tool: Some(fallback_tool.to_string()),
            reason: None,
        };
    };
    let code = sc
        .get("code")
        .and_then(Value::as_str)
        .and_then(|s| s.parse().ok())
        .unwrap_or(GatewayErrorCode::UpstreamError);
    let message = sc
        .get("message")
        .and_then(Value::as_str)
        .map(str::to_string)
        .unwrap_or_else(|| {
            if text.is_empty() {
                code.to_string()
            } else {
                text
            }
        });
    let retryable = sc
        .get("retryable")
        .and_then(Value::as_bool)
        .unwrap_or_else(|| code.retryable());
    let server = sc
        .get("server")
        .and_then(Value::as_str)
        .map(str::to_string)
        .or_else(|| Some(fallback_server.to_string()).filter(|s| !s.is_empty()));
    let tool = sc
        .get("tool")
        .and_then(Value::as_str)
        .map(str::to_string)
        .or_else(|| Some(fallback_tool.to_string()).filter(|t| !t.is_empty()));
    let reason = sc.get("reason").and_then(Value::as_str).map(str::to_string);
    GatewayError::Tool {
        code,
        message,
        retryable,
        server,
        tool,
        reason,
    }
}

/// A successful tool result: structured payload when present, text always.
#[derive(Debug, Clone, PartialEq)]
pub struct ToolOutput {
    pub structured: Option<Value>,
    pub text: String,
}

// ---------------------------------------------------------------------------
// Gateway connection
// ---------------------------------------------------------------------------

/// A connection to a Cerberus gateway (stdio or streamable HTTP).
pub struct Gateway {
    service: RunningService<RoleClient, ()>,
}

impl Gateway {
    /// Spawn `cerberus gateway start --transport stdio` and speak MCP over it.
    pub async fn connect_stdio(binary: &str) -> Result<Self, Error> {
        Self::connect_stdio_with(binary, &[]).await
    }

    /// Like [`Self::connect_stdio`], with extra CLI args (e.g. `--profile`).
    pub async fn connect_stdio_with(binary: &str, extra: &[&str]) -> Result<Self, Error> {
        let mut cmd = tokio::process::Command::new(binary);
        cmd.arg("gateway")
            .arg("start")
            .arg("--transport")
            .arg("stdio");
        for arg in extra {
            cmd.arg(arg);
        }
        let transport = TokioChildProcess::new(cmd.configure(|_| {}))
            .map_err(|e| Error::Connect(e.to_string()))?;
        let service = tokio::time::timeout(DEFAULT_CONNECT_TIMEOUT, ().serve(transport))
            .await
            .map_err(|_| Error::Connect("timed out during initialize".into()))?
            .map_err(|e| Error::Connect(e.to_string()))?;
        Ok(Self { service })
    }

    /// Connect to a streamable-HTTP gateway endpoint.
    pub async fn connect_http(url: &str, bearer: Option<String>) -> Result<Self, Error> {
        let mut config = StreamableHttpClientTransportConfig::with_uri(url.to_string());
        if let Some(token) = bearer {
            config.auth_header = Some(format!("Bearer {token}"));
        }
        let transport = StreamableHttpClientTransport::from_config(config);
        let service = tokio::time::timeout(DEFAULT_CONNECT_TIMEOUT, ().serve(transport))
            .await
            .map_err(|_| Error::Connect("timed out during initialize".into()))?
            .map_err(|e| Error::Connect(e.to_string()))?;
        Ok(Self { service })
    }

    /// The underlying rmcp client, for APIs this SDK does not wrap yet.
    pub fn service(&self) -> &RunningService<RoleClient, ()> {
        &self.service
    }

    /// List tools with upstream coordinates resolved from `_meta`.
    pub async fn list_tools(&self) -> Result<Vec<RoutedTool>, Error> {
        let result = self
            .service
            .list_tools(None)
            .await
            .map_err(|e| Error::Protocol(e.to_string()))?;
        result.tools.into_iter().map(RoutedTool::try_from).collect()
    }

    /// Call `tool` on upstream `server` through the gateway.
    pub async fn call(
        &self,
        server: &str,
        tool: &str,
        arguments: Value,
    ) -> Result<ToolOutput, GatewayError> {
        let name = public_tool_name(server, tool);
        let args = match arguments {
            Value::Null => None,
            Value::Object(map) => Some(map),
            other => Some(JsonObject::from_iter([("value".to_string(), other)])),
        };
        let mut params = CallToolRequestParams::new(name);
        if let Some(args) = args {
            params = params.with_arguments(args);
        }
        let result = self
            .service
            .call_tool(params)
            .await
            .map_err(|e| GatewayError::Local(Error::Protocol(e.to_string())))?;
        if result.is_error.unwrap_or(false) {
            return Err(gateway_error(result, server, tool));
        }
        let text = text_of(&result);
        Ok(ToolOutput {
            structured: result.structured_content,
            text,
        })
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use rmcp::model::MetaObject;

    fn tool_with_meta(name: &str, server: &str, tool: &str) -> Tool {
        let schema = serde_json::json!({"type": "object", "properties": {}});
        let mut t = Tool::new(
            name.to_string(),
            format!("[{server}] {tool}"),
            std::sync::Arc::new(schema.as_object().unwrap().clone()),
        );
        let mut route = JsonObject::new();
        route.insert("server".into(), Value::String(server.into()));
        route.insert("tool".into(), Value::String(tool.into()));
        let mut meta = JsonObject::new();
        meta.insert(ROUTE_META_KEY.into(), Value::Object(route));
        t.meta = Some(MetaObject(meta));
        t
    }

    #[test]
    fn route_meta_is_authoritative() {
        let t = RoutedTool::try_from(tool_with_meta(
            "fs__read_file",
            "remote-filesystem",
            "read_file",
        ))
        .unwrap();
        assert_eq!(t.server, "remote-filesystem");
        assert_eq!(t.tool, "read_file");
        assert_eq!(t.name, "fs__read_file");
        assert_eq!(
            public_tool_name(&t.server, &t.tool),
            "remote-filesystem__read_file"
        );
    }

    #[test]
    fn falls_back_to_first_separator_when_meta_missing() {
        let mut t = tool_with_meta("srv__do__it", "srv", "do__it");
        t.meta = None;
        let routed = RoutedTool::try_from(t).unwrap();
        assert_eq!(routed.server, "srv");
        assert_eq!(routed.tool, "do__it");
    }

    #[test]
    fn error_envelope_parses_to_typed_code() {
        let mut result = CallToolResult::error(vec![ContentBlock::text(
            "policy denied: deny pattern matched",
        )]);
        result.structured_content = Some(serde_json::json!({
            "code": "policy_denied",
            "message": "policy denied: deny pattern matched",
            "retryable": false,
            "exit_code": 6,
            "server": "github",
            "tool": "search_repos",
            "reason": "deny pattern matched"
        }));
        match gateway_error(result, "github", "search_repos") {
            GatewayError::Tool {
                code,
                retryable,
                server,
                tool,
                reason,
                ..
            } => {
                assert_eq!(code, GatewayErrorCode::PolicyDenied);
                assert!(!retryable);
                assert_eq!(server.as_deref(), Some("github"));
                assert_eq!(tool.as_deref(), Some("search_repos"));
                assert_eq!(reason.as_deref(), Some("deny pattern matched"));
            }
            other => panic!("expected tool error, got {other}"),
        }
    }

    #[test]
    fn error_without_structured_content_is_upstream_error() {
        let result = CallToolResult::error(vec![ContentBlock::text("boom")]);
        match gateway_error(result, "fs", "read") {
            GatewayError::Tool {
                code,
                retryable,
                server,
                ..
            } => {
                assert_eq!(code, GatewayErrorCode::UpstreamError);
                assert!(!retryable);
                assert_eq!(server.as_deref(), Some("fs"));
            }
            other => panic!("expected tool error, got {other}"),
        }
    }

    #[test]
    fn codes_round_trip_on_the_wire() {
        for code in [
            GatewayErrorCode::UnknownTool,
            GatewayErrorCode::ServerNotFound,
            GatewayErrorCode::PolicyDenied,
            GatewayErrorCode::ConfirmationRequired,
            GatewayErrorCode::UpstreamUnavailable,
            GatewayErrorCode::ShuttingDown,
            GatewayErrorCode::ConcurrencySaturated,
            GatewayErrorCode::Timeout,
            GatewayErrorCode::UpstreamError,
        ] {
            let wire = serde_json::to_value(code).unwrap();
            assert_eq!(wire, Value::String(code.as_str().to_string()));
            let back: GatewayErrorCode = serde_json::from_value(wire).unwrap();
            assert_eq!(back, code);
            assert_eq!(code.to_string(), code.as_str());
        }
    }

    #[test]
    fn retryable_and_exit_code_table_matches_contract() {
        use GatewayErrorCode as C;
        let cases: [(C, bool, u8); 9] = [
            (C::UnknownTool, false, 5),
            (C::ServerNotFound, false, 5),
            (C::PolicyDenied, false, 6),
            (C::ConfirmationRequired, false, 6),
            (C::Timeout, false, 7),
            (C::ConcurrencySaturated, true, 7),
            (C::UpstreamUnavailable, true, 1),
            (C::ShuttingDown, true, 1),
            (C::UpstreamError, false, 1),
        ];
        for (code, retryable, exit) in cases {
            assert_eq!(code.retryable(), retryable, "{code}");
            assert_eq!(code.exit_code(), exit, "{code}");
        }
    }

    #[test]
    fn success_output_exposes_structured_and_text() {
        let mut result = CallToolResult::success(vec![ContentBlock::text("hello")]);
        result.structured_content = Some(serde_json::json!({"n": 1}));
        let text = text_of(&result);
        let output = ToolOutput {
            structured: result.structured_content,
            text,
        };
        assert_eq!(output.text, "hello");
        assert_eq!(output.structured, Some(serde_json::json!({"n": 1})));
    }
}
