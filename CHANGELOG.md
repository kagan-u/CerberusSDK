# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

## [0.1.0] - 2026-10-25

### Added

- First release of the Cerberus SDKs in four languages, each wrapping the
  official (or community) MCP client with a small `cerberus::Gateway` /
  `Gateway` facade:
  - `rs/` — Rust, wraps `rmcp`
  - `ts/` — TypeScript/JavaScript, wraps `@modelcontextprotocol/sdk`
  - `py/` — Python, wraps `mcp`
  - `cpp/` — C++, wraps [`Neumann-Labs/mcp-cpp`](https://github.com/Neumann-Labs/mcp-cpp)
- Transports in all four languages: spawn/attach the gateway over stdio
  (`connect_stdio`, TS `connectStdio`) and streamable HTTP with bearer
  auth (`connect_http`, TS `connectHttp`).
- Route resolution from `_meta["cerberus.dev/route"]` — SDKs never parse
  the `{server}__{tool}` public name themselves.
- Structured gateway errors: `policy_denied` and the other wire codes map
  to a typed error (`GatewayError` / `GatewayErrorCode`) carrying
  `retryable`, `exit_code`, `server`, `tool` and `reason`.
- End-to-end test harnesses in all four languages against a real
  `cerberus gateway start` (stdio and bearer-authenticated HTTP) with a
  shared fixture contract (`CERBERUS_BIN` / `CERBERUS_FAKE_UPSTREAM_BIN`),
  plus a CI `e2e` job that builds a fresh CerberusMCP checkout and runs
  every harness.
- `docs/contract.md` — the wire contract all SDKs are tested against.
