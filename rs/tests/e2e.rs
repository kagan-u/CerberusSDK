//! End-to-end: real `cerberus` binary + fake upstream + this SDK over stdio.
//!
//! Verifies `docs/contract.md` against a live gateway:
//! - `_meta["cerberus.dev/route"]` on every `tools/list` entry (raw and resolved),
//! - policy filtering of `tools/list`,
//! - a successful round trip through `tools/call`,
//! - the `policy_denied` error envelope (`retryable: false`, `exit_code: 6`).
//!
//! Skips (and passes) when no `cerberus` binary is available: set `CERBERUS_BIN`
//! or build CerberusMCP at `../CerberusMCP/target/debug/cerberus`.

use cerberus_sdk::{Gateway, GatewayError, GatewayErrorCode, ROUTE_META_KEY};
use serde_json::json;
use std::path::PathBuf;

fn cerberus_bin() -> Option<PathBuf> {
    if let Ok(p) = std::env::var("CERBERUS_BIN") {
        let p = PathBuf::from(p);
        if p.is_file() {
            return Some(p);
        }
    }
    // repo root is one level up from rs/; CerberusMCP is a sibling checkout.
    let sibling =
        PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("../../CerberusMCP/target/debug/cerberus");
    if sibling.is_file() {
        return Some(sibling);
    }
    None
}

fn fake_upstream_bin() -> Option<PathBuf> {
    if let Ok(p) = std::env::var("CERBERUS_FAKE_UPSTREAM_BIN") {
        let p = PathBuf::from(p);
        if p.is_file() {
            return Some(p);
        }
    }
    // tests run as target/debug/deps/e2e-<hash>; the example lands next to it.
    let exe = std::env::current_exe().ok()?;
    let debug_dir = exe.parent()?.parent()?;
    let candidate = debug_dir.join("examples").join("fake_upstream");
    candidate.is_file().then_some(candidate)
}

/// `cargo test` does not build examples, so build it on demand (deps are
/// already compiled by the test run itself).
fn ensure_example() -> Option<PathBuf> {
    if let Some(p) = fake_upstream_bin() {
        return Some(p);
    }
    let cargo = std::env::var("CARGO").unwrap_or_else(|_| "cargo".into());
    let ok = std::process::Command::new(cargo)
        .args(["build", "--example", "fake_upstream"])
        .arg("--manifest-path")
        .arg(format!("{}/Cargo.toml", env!("CARGO_MANIFEST_DIR")))
        .status()
        .map(|s| s.success())
        .unwrap_or(false);
    ok.then(fake_upstream_bin).flatten()
}

#[tokio::test(flavor = "multi_thread", worker_threads = 2)]
async fn gateway_stdio_roundtrip() {
    let Some(cerberus) = cerberus_bin() else {
        eprintln!("e2e skipped: no cerberus binary (set CERBERUS_BIN or build ../CerberusMCP)");
        return;
    };
    let upstream = ensure_example()
        .unwrap_or_else(|| {
            panic!("fake_upstream example missing and `cargo build --example` failed")
        })
        .canonicalize()
        .expect("canonical upstream path")
        .display()
        .to_string();

    let stamp = std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .unwrap()
        .as_nanos();
    let root =
        std::env::temp_dir().join(format!("cerberus-sdk-e2e-{}-{stamp}", std::process::id()));
    let config = root.join("config");
    let data = root.join("data");
    std::fs::create_dir_all(&config).unwrap();
    std::fs::create_dir_all(&data).unwrap();

    let up = upstream.replace('\\', "\\\\");
    let registry = format!(
        r#"
schema_version = 2

[settings]
default_mode = "direct"

[servers.echo-server]
description = "e2e echo upstream"
timeout_ms = 15000

[servers.echo-server.transport]
type = "stdio"
command = "{up}"
args = []
env = {{ CERBERUS_FAKE_UPSTREAM = {{ source = "literal", value = "1" }} }}

[servers.echo-server.policy]
deny_tools = []

[servers.locked]
description = "e2e upstream whose echo tool is denied"
timeout_ms = 15000

[servers.locked.transport]
type = "stdio"
command = "{up}"
args = []

[servers.locked.policy]
deny_tools = ["echo"]
"#
    );
    std::fs::write(config.join("conf.cerberus"), registry).unwrap();

    // SAFETY: this test binary contains exactly this one test, so no other
    // thread in this process reads or writes the process environment.
    unsafe {
        std::env::set_var("CERBERUS_CONFIG_DIR", &config);
        std::env::set_var("CERBERUS_DATA_DIR", &data);
    }

    let gw = Gateway::connect_stdio(cerberus.to_str().expect("utf-8 cerberus path"))
        .await
        .expect("gateway connect");

    // Raw wire: every listed tool carries _meta["cerberus.dev/route"].
    let raw = gw.service().list_tools(None).await.expect("raw list_tools");
    let raw_echo = raw
        .tools
        .iter()
        .find(|t| t.name == "echo-server__echo")
        .expect("echo tool listed");
    let meta = raw_echo.meta.as_ref().expect("tool must carry _meta");
    let route = meta
        .0
        .get(ROUTE_META_KEY)
        .unwrap_or_else(|| panic!("`{ROUTE_META_KEY}` missing in _meta"));
    assert_eq!(
        route.get("server").and_then(|v| v.as_str()),
        Some("echo-server")
    );
    assert_eq!(route.get("tool").and_then(|v| v.as_str()), Some("echo"));

    // SDK resolution matches, and the denied tool is filtered from the list.
    let tools = gw.list_tools().await.expect("routed list_tools");
    let routed = tools
        .iter()
        .find(|t| t.name == "echo-server__echo")
        .expect("routed echo tool");
    assert_eq!(routed.server, "echo-server");
    assert_eq!(routed.tool, "echo");
    assert!(
        tools.iter().all(|t| !t.name.starts_with("locked__")),
        "denied tools must not be listed: {tools:?}"
    );

    // Successful round trip through the upstream.
    let out = gw
        .call("echo-server", "echo", json!({"text": "hi"}))
        .await
        .expect("echo call");
    assert_eq!(out.text, "echo: hi");

    // Policy denial surfaces as a typed envelope, not prose.
    match gw.call("locked", "echo", json!({"text": "hi"})).await {
        Err(GatewayError::Tool {
            code,
            retryable,
            server,
            tool,
            reason,
            ..
        }) => {
            assert_eq!(code, GatewayErrorCode::PolicyDenied);
            assert_eq!(code.exit_code(), 6);
            assert!(!retryable);
            assert_eq!(server.as_deref(), Some("locked"));
            assert_eq!(tool.as_deref(), Some("echo"));
            assert!(reason.is_some(), "deny reason expected in envelope");
        }
        other => panic!("expected policy_denied tool error, got {other:?}"),
    }

    drop(gw);
    // The gateway writes its state file on shutdown; give it time to finish
    // before removing the directory it runs from.
    tokio::time::sleep(std::time::Duration::from_millis(500)).await;
    let _ = std::fs::remove_dir_all(&root);
}
