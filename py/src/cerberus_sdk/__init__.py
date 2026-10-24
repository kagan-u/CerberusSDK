"""Python SDK for the CerberusMCP gateway.

Wraps the official ``mcp`` client with two Cerberus additions:

- :class:`RoutedTool`: every listed tool resolves its upstream ``server`` /
  ``tool`` from ``_meta["cerberus.dev/route"]`` — no ``{server}__{tool}``
  splitting.
- :class:`GatewayError`: failing calls raise a typed error carrying the
  gateway ``code``, ``retryable``, and the CLI-aligned ``exit_code``.

Wire contract: ``../docs/contract.md``.
"""

from __future__ import annotations

from dataclasses import dataclass, field
from enum import Enum
from typing import Any

from mcp import Client, Implementation, StdioServerParameters
from mcp.client.streamable_http import create_mcp_http_client, streamable_http_client
from mcp.types import CallToolResult, ListToolsResult, Tool

__all__ = [
    "ROUTE_META_KEY",
    "Gateway",
    "GatewayConnectOptions",
    "GatewayError",
    "GatewayErrorCode",
    "RoutedTool",
    "ToolOutput",
    "exit_code",
    "gateway_error",
    "is_retryable",
    "public_tool_name",
    "routed_tool",
]

#: Reverse-DNS ``_meta`` key carrying ``{server, tool}`` on every listed tool.
ROUTE_META_KEY = "cerberus.dev/route"

_DEFAULT_CONNECT_TIMEOUT = 30.0
_DEFAULT_CLIENT_INFO = Implementation(name="cerberus-sdk", version="0.1.0")


class GatewayErrorCode(str, Enum):
    """Stable gateway error codes (snake_case on the wire)."""

    UNKNOWN_TOOL = "unknown_tool"
    SERVER_NOT_FOUND = "server_not_found"
    POLICY_DENIED = "policy_denied"
    CONFIRMATION_REQUIRED = "confirmation_required"
    UPSTREAM_UNAVAILABLE = "upstream_unavailable"
    SHUTTING_DOWN = "shutting_down"
    CONCURRENCY_SATURATED = "concurrency_saturated"
    TIMEOUT = "timeout"
    UPSTREAM_ERROR = "upstream_error"


def is_retryable(code: GatewayErrorCode) -> bool:
    """Whether an SDK may automatically retry a call that failed this way."""
    return code in {
        GatewayErrorCode.UPSTREAM_UNAVAILABLE,
        GatewayErrorCode.SHUTTING_DOWN,
        GatewayErrorCode.CONCURRENCY_SATURATED,
    }


def exit_code(code: GatewayErrorCode) -> int:
    """Process exit code the ``cerberus`` CLI reports for the same condition."""
    if code in (GatewayErrorCode.UNKNOWN_TOOL, GatewayErrorCode.SERVER_NOT_FOUND):
        return 5
    if code in (GatewayErrorCode.POLICY_DENIED, GatewayErrorCode.CONFIRMATION_REQUIRED):
        return 6
    if code in (GatewayErrorCode.TIMEOUT, GatewayErrorCode.CONCURRENCY_SATURATED):
        return 7
    return 1


class GatewayError(Exception):
    """A gateway-reported tool failure (from an ``isError: true`` result)."""

    def __init__(
        self,
        code: GatewayErrorCode,
        message: str,
        *,
        retryable: bool | None = None,
        server: str | None = None,
        tool: str | None = None,
        reason: str | None = None,
    ) -> None:
        super().__init__(f"{code.value}: {message}")
        self.code = code
        self.message = message
        self.retryable = is_retryable(code) if retryable is None else retryable
        self.server = server
        self.tool = tool
        self.reason = reason
        self.exit_code = exit_code(code)


@dataclass(frozen=True)
class RoutedTool:
    """A tool as listed by the gateway, with upstream coordinates resolved."""

    #: Public name for ``tools/call`` (``{server}__{tool}``).
    name: str
    server: str
    #: The tool's own name on its upstream server.
    tool: str
    description: str | None = None
    input_schema: Any = field(default_factory=dict)


@dataclass(frozen=True)
class ToolOutput:
    """A successful tool result: structured payload when present, text always."""

    text: str
    structured: Any = None


def public_tool_name(server: str, tool: str) -> str:
    """Public name for an upstream tool."""
    return f"{server}__{tool}"


def _text_of(blocks: list[Any]) -> str:
    parts: list[str] = []
    for block in blocks:
        text = getattr(block, "text", None)
        if isinstance(text, str):
            parts.append(text)
    return "\n".join(parts)


def routed_tool(tool: Tool) -> RoutedTool:
    """Resolve a listed tool's upstream coordinates.

    Prefers ``_meta["cerberus.dev/route"]``; falls back to first-``__`` name
    parsing for pre-contract servers. Raises ``ValueError`` when neither is
    possible.
    """
    meta = tool.meta or {}
    route = meta.get(ROUTE_META_KEY)
    if isinstance(route, dict):
        server, name = route.get("server"), route.get("tool")
        if isinstance(server, str) and isinstance(name, str):
            return RoutedTool(
                name=tool.name,
                server=server,
                tool=name,
                description=tool.description,
                input_schema=tool.input_schema,
            )
    sep = tool.name.find("__")
    if 0 < sep < len(tool.name) - 2:
        return RoutedTool(
            name=tool.name,
            server=tool.name[:sep],
            tool=tool.name[sep + 2 :],
            description=tool.description,
            input_schema=tool.input_schema,
        )
    raise ValueError(
        f"tool `{tool.name}` has no route metadata and no `__` separator"
    )


def gateway_error(
    result: CallToolResult,
    fallback_server: str,
    fallback_tool: str,
) -> GatewayError:
    """Build a :class:`GatewayError` from an ``isError: true`` result.

    Prefers ``structuredContent`` and degrades to ``upstream_error`` without it.
    """
    text = _text_of(result.content)
    sc = result.structured_content
    if not isinstance(sc, dict):
        return GatewayError(
            GatewayErrorCode.UPSTREAM_ERROR,
            text or "tool call failed",
            server=fallback_server,
            tool=fallback_tool,
        )
    raw_code = sc.get("code")
    try:
        code = GatewayErrorCode(raw_code)
    except ValueError:
        code = GatewayErrorCode.UPSTREAM_ERROR
    message = sc.get("message")
    if not isinstance(message, str) or not message:
        message = text or code.value
    server = sc.get("server") if isinstance(sc.get("server"), str) else fallback_server
    tool = sc.get("tool") if isinstance(sc.get("tool"), str) else fallback_tool
    reason = sc.get("reason") if isinstance(sc.get("reason"), str) else None
    retryable = sc.get("retryable") if isinstance(sc.get("retryable"), bool) else None
    return GatewayError(
        code,
        message,
        retryable=retryable,
        server=server,
        tool=tool,
        reason=reason,
    )


class Gateway:
    """A connection to a Cerberus gateway (stdio or streamable HTTP)."""

    def __init__(self, client: Client) -> None:
        self._client = client
        self._closed = False

    # -- construction -------------------------------------------------------

    @classmethod
    async def connect_stdio(
        cls,
        command: str = "cerberus",
        *,
        args: tuple[str, ...] = (),
        env: dict[str, str] | None = None,
        client_info: Implementation | None = None,
    ) -> Gateway:
        """Spawn ``cerberus gateway start --transport stdio`` and speak MCP."""
        params = StdioServerParameters(
            command=command,
            args=("gateway", "start", "--transport", "stdio", *args),
            env=env,
        )
        client = Client(params, client_info=client_info or _DEFAULT_CLIENT_INFO)
        await client.__aenter__()
        return cls(client)

    @classmethod
    async def connect_http(
        cls,
        url: str,
        *,
        bearer: str | None = None,
        client_info: Implementation | None = None,
    ) -> Gateway:
        """Connect to a streamable-HTTP gateway endpoint."""
        http_client = (
            create_mcp_http_client(headers={"authorization": f"Bearer {bearer}"})
            if bearer
            else None
        )
        transport = streamable_http_client(url, http_client=http_client)
        client = Client(transport, client_info=client_info or _DEFAULT_CLIENT_INFO)
        await client.__aenter__()
        return cls(client)

    @property
    def client(self) -> Client:
        """The underlying official ``mcp`` client."""
        return self._client

    # -- operations ---------------------------------------------------------

    async def list_tools(self) -> list[RoutedTool]:
        """List tools with upstream coordinates resolved from ``_meta``."""
        result: ListToolsResult = await self._client.list_tools()
        return [routed_tool(tool) for tool in result.tools]

    async def call(
        self,
        server: str,
        tool: str,
        arguments: dict[str, Any] | None = None,
    ) -> ToolOutput:
        """Call ``tool`` on upstream ``server`` through the gateway."""
        result = await self._client.call_tool(
            public_tool_name(server, tool), arguments or {}
        )
        if result.is_error:
            raise gateway_error(result, server, tool)
        return ToolOutput(
            text=_text_of(result.content),
            structured=result.structured_content,
        )

    async def close(self) -> None:
        """Close the connection (and any spawned gateway process)."""
        if not self._closed:
            self._closed = True
            await self._client.__aexit__(None, None, None)

    async def __aenter__(self) -> Gateway:
        return self

    async def __aexit__(self, *exc_info: object) -> None:
        await self.close()
