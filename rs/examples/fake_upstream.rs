//! Minimal MCP stdio server used by the e2e tests.
//!
//! Exposes one tool, `echo(text)`, which replies `echo: <text>`. The registry
//! fixture in `tests/e2e.rs` points the real `cerberus` gateway at this binary.

use std::sync::Arc;

use rmcp::{
    RoleServer, ServerHandler, ServiceExt,
    model::{
        CallToolRequestParams, CallToolResponse, CallToolResult, ContentBlock,
        ErrorData as McpError, JsonObject, ListToolsResult, PaginatedRequestParams,
        ServerCapabilities, ServerConfig, Tool,
    },
    service::RequestContext,
};

#[derive(Clone)]
struct FakeUpstream;

impl ServerHandler for FakeUpstream {
    fn get_info(&self) -> ServerConfig {
        ServerConfig::new(ServerCapabilities::builder().enable_tools().build())
            .with_instructions("Cerberus e2e fake upstream")
    }

    async fn list_tools(
        &self,
        _request: Option<PaginatedRequestParams>,
        _context: RequestContext<RoleServer>,
    ) -> Result<ListToolsResult, McpError> {
        let mut schema = JsonObject::new();
        schema.insert("type".into(), serde_json::json!("object"));
        schema.insert(
            "properties".into(),
            serde_json::json!({"text": {"type": "string"}}),
        );
        schema.insert("required".into(), serde_json::json!(["text"]));
        let tool = Tool::new(
            "echo".to_string(),
            "Echo the given text".to_string(),
            Arc::new(schema),
        );
        Ok(ListToolsResult::with_all_items(vec![tool]))
    }

    async fn call_tool(
        &self,
        request: CallToolRequestParams,
        _context: RequestContext<RoleServer>,
    ) -> Result<CallToolResponse, McpError> {
        let text = request
            .arguments
            .as_ref()
            .and_then(|a| a.get("text"))
            .and_then(|v| v.as_str())
            .unwrap_or("")
            .to_string();
        let result = CallToolResult::success(vec![ContentBlock::text(format!("echo: {text}"))]);
        Ok(result.into())
    }
}

#[tokio::main]
async fn main() -> Result<(), Box<dyn std::error::Error>> {
    let service = FakeUpstream.serve(rmcp::transport::stdio()).await?;
    let _ = service.waiting().await;
    Ok(())
}
