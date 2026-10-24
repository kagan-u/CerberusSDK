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
| C++ | [`cpp/`](cpp/) | [Neumann-Labs/mcp-cpp](https://github.com/Neumann-Labs/mcp-cpp) (no official C++ SDK exists) | planned |

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
```

CI runs per-language jobs; the end-to-end tests need a `cerberus` binary and
are skipped when it is not on `PATH`.
