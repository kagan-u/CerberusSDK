# CerberusSDK

Language SDKs for the [CerberusMCP](https://github.com/kagan-u/CerberusMCP)
gateway. Each SDK wraps the **official MCP client** for its language and adds
two things on top: structured tool routing and typed gateway errors.

Wire contract: [`docs/contract.md`](docs/contract.md) (source of truth:
`CerberusMCP/docs/gateway.md`).

## Languages

| Language | Directory | Wraps | Status |
| --- | --- | --- | --- |
| Rust | [`rs/`](rs/) | `rmcp` (official Rust SDK) | **implemented** |
| TypeScript / JavaScript | [`ts/`](ts/) | `@modelcontextprotocol/client` (one package serves both) | **implemented** |
| Python | [`py/`](py/) | `mcp` (official Python SDK) | **implemented** |
| C++ | [`cpp/`](cpp/) | [Neumann-Labs/mcp-cpp](https://github.com/Neumann-Labs/mcp-cpp) v0.1.0 (no official C++ SDK exists) | **implemented** (stdio is POSIX-only) |

## What you get

- **`_meta["cerberus.dev/route"]`** — every listed tool carries its upstream
  `server`/`tool`; SDKs never split `{server}__{tool}` names themselves.
- **Typed errors** — `isError` results parse into a code enum
  (`policy_denied`, `timeout`, ...) with `retryable` and CLI-aligned
  `exit_code`.
- **Connections** — stdio (spawn `cerberus gateway start --transport stdio`)
  or streamable HTTP with bearer auth.

## TypeScript example

```ts
import { Gateway } from "cerberus-sdk";

const gw = await Gateway.connectStdio("cerberus");
for (const t of await gw.listTools()) console.log(t.name, t.server, t.tool);
try {
  const out = await gw.call("github", "search_repos", { q: "mcp" });
  console.log(out.text, out.structured);
} catch (e) {
  if (e instanceof GatewayError && e.retryable) { /* back off and retry */ }
}
await gw.close();
```

## C++ example

```cpp
#include <cerberus/sdk.hpp>

auto gw = cerberus::Gateway::connect_stdio();
for (const auto& t : gw.list_tools())
    std::cout << t.name << " -> " << t.server << "::" << t.tool << "\n";
try {
    auto out = gw.call("github", "search_repos", {{"q", "mcp"}});
} catch (const cerberus::GatewayError& e) {
    if (e.retryable) { /* back off and retry */ }
}
gw.disconnect();
```

## Python example

```rust
let gw = cerberus_sdk::Gateway::connect_stdio("cerberus").await?;
for t in gw.list_tools().await? {
    println!("{} -> {} :: {}", t.name, t.server, t.tool);
}
match gw.call("github", "search_repos", serde_json::json!({"q":"mcp"})).await? {
    out => println!("{} {}", out.text, out.structured.unwrap_or_default()),
}
# Err::<(), cerberus_sdk::Error>(cerberus_sdk::Error::Connect("example".into()))
```

## Development

```sh
cargo test --manifest-path rs/Cargo.toml   # Rust
cd ts && npm ci && npm run typecheck && npm test && npm run build
cd py && pip install -e ".[dev]" && pytest
cmake -S cpp -B cpp/build && cmake --build cpp/build && ctest --test-dir cpp/build
```

### End-to-end tests (all four SDKs)

Every SDK has a live round trip against a **real** `cerberus gateway start
--transport stdio`, sharing one fixture set:

- `rs/tests/e2e.rs` — `cargo test --test e2e`
- `ts/test/e2e.test.ts` — `npx vitest run` (runs with the unit tests)
- `py/tests/test_e2e.py` — `pytest` (runs with the unit tests)
- `cpp/tests/e2e.cpp` — `ctest` (registered as `cerberus_e2e`)

Each verifies route resolution from `_meta["cerberus.dev/route"]`, policy
filtering out of `tools/list`, a live `tools/call` round trip against the
fake upstream (`rs/examples/fake_upstream.rs`), and the `policy_denied`
error envelope (code/retryable/exit code/server/tool). Fixtures resolve from
`CERBERUS_BIN` / `CERBERUS_FAKE_UPSTREAM_BIN`, falling back to sibling
checkouts (`../CerberusMCP/target/debug/cerberus`,
`../rs/target/debug/examples/fake_upstream`). Tests skip (and pass) when
fixtures are missing; `CERBERUS_REQUIRE_E2E=1` — set by the CI `e2e` job,
which builds both fixtures once and runs all four harnesses — turns missing
fixtures into failures.

The C++ stdio transport is POSIX-only (an `mcp-cpp` limitation); build with
`-DCERBERUS_ENABLE_HTTP=OFF` to skip the cpp-httplib dependency when only
stdio is needed.

CI runs per-language jobs; the end-to-end tests need a `cerberus` binary and
are skipped when it is not on `PATH`.
