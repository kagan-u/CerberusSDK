"""End-to-end against a real ``cerberus gateway start --transport stdio``.

Mirrors ``rs/tests/e2e.rs``. Skips when fixtures are missing — provide them
via ``CERBERUS_BIN`` / ``CERBERUS_FAKE_UPSTREAM_BIN`` or a sibling checkout at
``../CerberusMCP`` + the ``fake_upstream`` example, or set
``CERBERUS_REQUIRE_E2E=1`` to make missing fixtures a failure (CI does).
"""

from __future__ import annotations

import asyncio
import json
import os
from pathlib import Path

import pytest

from cerberus_sdk import Gateway, GatewayError, GatewayErrorCode

PY_DIR = Path(__file__).resolve().parents[1]
REPO = PY_DIR.parent

CERBERUS = Path(
    os.environ.get(
        "CERBERUS_BIN", REPO.parent / "CerberusMCP" / "target" / "debug" / "cerberus"
    )
)
UPSTREAM = Path(
    os.environ.get(
        "CERBERUS_FAKE_UPSTREAM_BIN",
        REPO / "rs" / "target" / "debug" / "examples" / "fake_upstream",
    )
)
HAVE_FIXTURES = CERBERUS.is_file() and UPSTREAM.is_file()
REQUIRE = os.environ.get("CERBERUS_REQUIRE_E2E") == "1"

pytestmark = pytest.mark.skipif(
    not HAVE_FIXTURES and not REQUIRE,
    reason=(
        "gateway/upstream fixtures missing "
        "(build CerberusMCP and `cargo build --example fake_upstream`)"
    ),
)

REGISTRY = """schema_version = 2

[settings]
default_mode = "direct"

[servers.echo-server]
description = "e2e echo upstream"
timeout_ms = 15000

[servers.echo-server.transport]
type = "stdio"
command = "__UPSTREAM__"
args = []
env = { CERBERUS_FAKE_UPSTREAM = { source = "literal", value = "1" } }

[servers.echo-server.policy]
deny_tools = []

[servers.locked]
description = "e2e upstream whose echo tool is denied"
timeout_ms = 15000

[servers.locked.transport]
type = "stdio"
command = "__UPSTREAM__"
args = []

[servers.locked.policy]
deny_tools = ["echo"]
"""


HTTP_TOKEN = "e2e-http-secret"


async def _spawn_http_gateway(config: Path) -> tuple[asyncio.subprocess.Process, str]:
    """Start the gateway over HTTP and return (process, url)."""
    process = await asyncio.create_subprocess_exec(
        str(CERBERUS),
        "gateway",
        "start",
        "--transport",
        "http",
        "--bind",
        "127.0.0.1:0",
        "--token",
        HTTP_TOKEN,
        env={
            **os.environ,
            "CERBERUS_CONFIG_DIR": str(config),
            "CERBERUS_DATA_DIR": str(config / "data"),
        },
        stdout=asyncio.subprocess.DEVNULL,
        stderr=asyncio.subprocess.DEVNULL,
    )
    # The state file names the resolved bind (port 0 -> real port) and lands
    # just before the server starts accepting.
    state_file = config / "data" / "state" / "gateway.json"
    for _ in range(300):
        try:
            bind = json.loads(state_file.read_text(encoding="utf-8")).get("bind", "")
        except OSError:
            bind = ""
        if bind and not bind.endswith(":0"):
            return process, f"http://{bind}/mcp"
        await asyncio.sleep(0.05)
    process.kill()
    raise AssertionError(f"gateway state file never reported a port: {state_file}")


async def test_gateway_http_bearer_roundtrip(tmp_path: Path) -> None:
    assert HAVE_FIXTURES, f"fixtures missing: {CERBERUS}, {UPSTREAM}"

    (tmp_path / "conf.cerberus").write_text(
        REGISTRY.replace("__UPSTREAM__", str(UPSTREAM.resolve())), encoding="utf-8"
    )
    process, url = await _spawn_http_gateway(tmp_path)
    try:
        # Missing and wrong tokens are rejected at the HTTP layer.
        with pytest.raises(Exception):
            await Gateway.connect_http(url)
        with pytest.raises(Exception):
            await Gateway.connect_http(url, bearer="wrong-secret")

        gw = await Gateway.connect_http(url, bearer=HTTP_TOKEN)
        try:
            tools = await gw.list_tools()
            echo = next(t for t in tools if t.name == "echo-server__echo")
            assert (echo.server, echo.tool) == ("echo-server", "echo")
            assert all(not t.name.startswith("locked__") for t in tools)

            out = await gw.call("echo-server", "echo", {"text": "hi"})
            assert out.text == "echo: hi"

            with pytest.raises(GatewayError) as exc:
                await gw.call("locked", "echo", {"text": "hi"})
            err = exc.value
            assert err.code == GatewayErrorCode.POLICY_DENIED
            assert err.retryable is False
            assert err.exit_code == 6
            assert err.server == "locked"
            assert err.tool == "echo"
        finally:
            await gw.close()
    finally:
        process.kill()
        await process.wait()


async def test_gateway_stdio_roundtrip(tmp_path: Path) -> None:
    assert HAVE_FIXTURES, f"fixtures missing: {CERBERUS}, {UPSTREAM}"

    (tmp_path / "conf.cerberus").write_text(
        REGISTRY.replace("__UPSTREAM__", str(UPSTREAM.resolve())), encoding="utf-8"
    )
    gw = await Gateway.connect_stdio(
        str(CERBERUS),
        env={
            "CERBERUS_CONFIG_DIR": str(tmp_path),
            "CERBERUS_DATA_DIR": str(tmp_path / "data"),
        },
    )
    try:
        tools = await gw.list_tools()
        echo = next(t for t in tools if t.name == "echo-server__echo")
        assert (echo.server, echo.tool) == ("echo-server", "echo")
        assert all(not t.name.startswith("locked__") for t in tools)

        out = await gw.call("echo-server", "echo", {"text": "hi"})
        assert out.text == "echo: hi"

        with pytest.raises(GatewayError) as exc:
            await gw.call("locked", "echo", {"text": "hi"})
        err = exc.value
        assert err.code == GatewayErrorCode.POLICY_DENIED
        assert err.retryable is False
        assert err.exit_code == 6
        assert err.server == "locked"
        assert err.tool == "echo"
    finally:
        await gw.close()
