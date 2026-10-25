//! End-to-end over streamable HTTP: real `cerberus gateway start --transport
//! http` + fake upstream + bearer auth, mirroring e2e.rs.
//!
//! Lives in its own test binary because e2e.rs mutates the process
//! environment (set_var) and cargo runs tests within one binary in parallel.
//!
//! Skips when fixtures are missing (CERBERUS_BIN / CERBERUS_FAKE_UPSTREAM_BIN
//! or sibling checkouts); CERBERUS_REQUIRE_E2E=1 makes that a failure.

use cerberus_sdk::{Gateway, GatewayError, GatewayErrorCode};
use serde_json::json;
use std::path::PathBuf;
use std::process::{Child, Command, Stdio};

const TOKEN: &str = "e2e-http-secret";

fn cerberus_bin() -> Option<PathBuf> {
    if let Ok(p) = std::env::var("CERBERUS_BIN") {
        let p = PathBuf::from(p);
        if p.is_file() {
            return Some(p);
        }
    }
    let sibling =
        PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("../../CerberusMCP/target/debug/cerberus");
    sibling.is_file().then_some(sibling)
}

fn fake_upstream_bin() -> Option<PathBuf> {
    if let Ok(p) = std::env::var("CERBERUS_FAKE_UPSTREAM_BIN") {
        let p = PathBuf::from(p);
        if p.is_file() {
            return Some(p);
        }
    }
    let exe = std::env::current_exe().ok()?;
    let debug_dir = exe.parent()?.parent()?;
    let candidate = debug_dir.join("examples").join("fake_upstream");
    candidate.is_file().then_some(candidate)
}

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

/// A spawned HTTP gateway plus the temp dirs it runs from.
struct HttpGateway {
    child: Child,
    url: String,
    root: PathBuf,
}

impl Drop for HttpGateway {
    fn drop(&mut self) {
        let _ = self.child.kill();
        let _ = self.child.wait();
        // See e2e.rs: the gateway touches its state dir during shutdown.
        std::thread::sleep(std::time::Duration::from_millis(500));
        let _ = std::fs::remove_dir_all(&self.root);
    }
}

/// Start `cerberus gateway start --transport http --bind 127.0.0.1:0 --token`
/// with the fixture registry and wait for the state file to report the port.
async fn spawn_http_gateway(cerberus: &str, upstream: &str) -> HttpGateway {
    let stamp = std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .unwrap()
        .as_nanos();
    let root = std::env::temp_dir().join(format!(
        "cerberus-sdk-e2e-http-{}-{stamp}",
        std::process::id()
    ));
    let config = root.join("config");
    let data = root.join("data");
    std::fs::create_dir_all(&config).unwrap();
    std::fs::create_dir_all(&data).unwrap();

    let registry = format!(
        r#"schema_version: 1

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
"#
    );
    std::fs::write(config.join("cerberus.yaml"), registry).unwrap();

    let child = Command::new(cerberus)
        .args([
            "gateway",
            "start",
            "--transport",
            "http",
            "--bind",
            "127.0.0.1:0",
            "--token",
            TOKEN,
        ])
        .env("CERBERUS_CONFIG_DIR", &config)
        .env("CERBERUS_DATA_DIR", &data)
        .stdout(Stdio::null())
        .stderr(Stdio::null())
        .spawn()
        .expect("spawn http gateway");

    // The state file names the bound address (port 0 resolves to a real port)
    // and appears just before axum starts accepting.
    let state_file = data.join("state").join("gateway.json");
    let endpoint = tokio::time::timeout(std::time::Duration::from_secs(15), async {
        loop {
            let bind = tokio::fs::read_to_string(&state_file)
                .await
                .ok()
                .and_then(|t| serde_json::from_str::<serde_json::Value>(&t).ok())
                .and_then(|v| v.get("bind").and_then(|b| b.as_str()).map(str::to_string))
                .filter(|b| !b.is_empty() && !b.ends_with(":0"));
            if let Some(bind) = bind {
                return bind;
            }
            tokio::time::sleep(std::time::Duration::from_millis(50)).await;
        }
    })
    .await
    .expect("gateway state file never reported a bound address");

    HttpGateway {
        child,
        url: format!("http://{endpoint}/mcp"),
        root,
    }
}

#[tokio::test(flavor = "multi_thread", worker_threads = 2)]
async fn gateway_http_bearer_roundtrip() {
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
    let cerberus = cerberus.to_str().expect("utf-8 cerberus path").to_string();

    let gw_http = spawn_http_gateway(&cerberus, &upstream).await;
    let url = gw_http.url.clone();

    // Missing and wrong tokens are rejected at the HTTP layer.
    for bad in [None, Some("wrong-secret")] {
        let result = Gateway::connect_http(&url, bad.map(str::to_string)).await;
        assert!(result.is_err(), "connect with {bad:?} must fail");
    }

    // The real token gets the full contract: route meta, policy filtering,
    // a live round trip, and the typed policy_denied envelope.
    let gw = Gateway::connect_http(&url, Some(TOKEN.to_string()))
        .await
        .expect("gateway connect with token");

    let tools = gw.list_tools().await.expect("routed list_tools");
    let routed = tools
        .iter()
        .find(|t| t.name == "echo-server__echo")
        .expect("echo tool");
    assert_eq!(routed.server, "echo-server");
    assert_eq!(routed.tool, "echo");
    assert!(tools.iter().all(|t| !t.name.starts_with("locked__")));

    let out = gw
        .call("echo-server", "echo", json!({"text": "hi"}))
        .await
        .expect("echo call");
    assert_eq!(out.text, "echo: hi");

    match gw.call("locked", "echo", json!({"text": "hi"})).await {
        Err(GatewayError::Tool {
            code,
            retryable,
            server,
            tool,
            ..
        }) => {
            assert_eq!(code, GatewayErrorCode::PolicyDenied);
            assert!(!retryable);
            assert_eq!(server.as_deref(), Some("locked"));
            assert_eq!(tool.as_deref(), Some("echo"));
        }
        other => panic!("expected policy_denied tool error, got {other:?}"),
    }

    drop(gw);
    drop(gw_http);
}
