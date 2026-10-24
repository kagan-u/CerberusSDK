from mcp.types import CallToolResult, TextContent, Tool

from cerberus_sdk import (
    ROUTE_META_KEY,
    GatewayError,
    GatewayErrorCode,
    exit_code,
    gateway_error,
    is_retryable,
    public_tool_name,
    routed_tool,
)


def fake_tool(name: str, route: dict[str, str] | None = None) -> Tool:
    meta = {ROUTE_META_KEY: route} if route else None
    return Tool(
        name=name,
        description="desc",
        input_schema={"type": "object", "properties": {}},
        meta=meta,
    )


def test_route_metadata_is_authoritative() -> None:
    routed = routed_tool(
        fake_tool(
            "fs__read_file",
            {"server": "remote-filesystem", "tool": "read_file"},
        )
    )
    assert routed.server == "remote-filesystem"
    assert routed.tool == "read_file"
    assert routed.name == "fs__read_file"
    assert public_tool_name(routed.server, routed.tool) == (
        "remote-filesystem__read_file"
    )


def test_falls_back_to_first_separator_without_metadata() -> None:
    routed = routed_tool(fake_tool("srv__do__it"))
    assert routed.server == "srv"
    assert routed.tool == "do__it"


def test_rejects_tools_with_neither_metadata_nor_separator() -> None:
    try:
        routed_tool(fake_tool("plain_tool"))
    except ValueError as exc:
        assert "no route metadata" in str(exc)
    else:
        raise AssertionError("expected ValueError")


def test_error_envelope_parses_to_typed_code() -> None:
    result = CallToolResult(
        content=[TextContent(text="policy denied: deny pattern matched")],
        is_error=True,
        structured_content={
            "code": "policy_denied",
            "message": "policy denied: deny pattern matched",
            "retryable": False,
            "exit_code": 6,
            "server": "github",
            "tool": "search_repos",
            "reason": "deny pattern matched",
        },
    )
    err = gateway_error(result, "github", "search_repos")
    assert isinstance(err, GatewayError)
    assert err.code is GatewayErrorCode.POLICY_DENIED
    assert err.retryable is False
    assert err.exit_code == 6
    assert err.server == "github"
    assert err.tool == "search_repos"
    assert err.reason == "deny pattern matched"
    assert "policy denied" in err.message
    assert "policy_denied" in str(err)


def test_error_without_structured_content_is_upstream_error() -> None:
    result = CallToolResult(
        content=[TextContent(text="boom")], is_error=True
    )
    err = gateway_error(result, "fs", "read")
    assert err.code is GatewayErrorCode.UPSTREAM_ERROR
    assert err.retryable is False
    assert err.exit_code == 1
    assert err.server == "fs"
    assert err.tool == "read"
    assert err.message == "boom"


def test_unknown_codes_degrade_but_keep_message() -> None:
    result = CallToolResult(
        content=[], is_error=True, structured_content={"code": "brand_new", "message": "hm"}
    )
    err = gateway_error(result, "s", "t")
    assert err.code is GatewayErrorCode.UPSTREAM_ERROR
    assert err.message == "hm"


def test_contract_table() -> None:
    table = {
        GatewayErrorCode.UNKNOWN_TOOL: (False, 5),
        GatewayErrorCode.SERVER_NOT_FOUND: (False, 5),
        GatewayErrorCode.POLICY_DENIED: (False, 6),
        GatewayErrorCode.CONFIRMATION_REQUIRED: (False, 6),
        GatewayErrorCode.TIMEOUT: (False, 7),
        GatewayErrorCode.CONCURRENCY_SATURATED: (True, 7),
        GatewayErrorCode.UPSTREAM_UNAVAILABLE: (True, 1),
        GatewayErrorCode.SHUTTING_DOWN: (True, 1),
        GatewayErrorCode.UPSTREAM_ERROR: (False, 1),
    }
    assert len(GatewayErrorCode) == len(table)
    for code, (retryable, code_exit) in table.items():
        assert is_retryable(code) is retryable, code
        assert exit_code(code) == code_exit, code
