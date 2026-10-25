"""End-to-end against a real ``cerberus gateway start --transport stdio``.

Mirrors ``rs/tests/e2e.rs``. Skips when fixtures are missing — provide them
via ``CERBERUS_BIN`` / ``CERBERUS_FAKE_UPSTREAM_BIN`` or a sibling checkout at
``../CerberusMCP`` + the ``fake_upstream`` example, or set
``CERBERUS_REQUIRE_E2E=1`` to make missing fixtures a failure (CI does).
"""

from __future__ import annotations

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

REGISTRY = """schema_version: 1

settings:
  default_mode: direct

servers:
  echo-server:
    description: e2e echo upstream
    transport:
      type: stdio
      command: '{upstream}'
      args: []
      env:
        CERBERUS_FAKE_UPSTREAM:
          source: literal
          value: "1"
    timeout_ms: 15000
    policy:
      deny_tools: []

  locked:
    description: e2e upstream whose echo tool is denied
    transport:
      type: stdio
      command: '{upstream}'
      args: []
    timeout_ms: 15000
    policy:
      deny_tools: ["echo"]
"""


async def test_gateway_stdio_roundtrip(tmp_path: Path) -> None:
    assert HAVE_FIXTURES, f"fixtures missing: {CERBERUS}, {UPSTREAM}"

    (tmp_path / "cerberus.yaml").write_text(
        REGISTRY.format(upstream=UPSTREAM.resolve()), encoding="utf-8"
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
