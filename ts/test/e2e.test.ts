import { spawn, type ChildProcess } from "node:child_process";
import {
  existsSync,
  mkdtempSync,
  readFileSync,
  rmSync,
  writeFileSync,
} from "node:fs";
import { tmpdir } from "node:os";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import { afterAll, describe, expect, it } from "vitest";
import { Gateway, GatewayError } from "../src/index.js";

/**
 * Live round trip against a real `cerberus gateway start --transport stdio`
 * (mirrors rs/tests/e2e.rs). Skips when fixtures are missing — provide them
 * via CERBERUS_BIN / CERBERUS_FAKE_UPSTREAM_BIN or a sibling checkout, or set
 * CERBERUS_REQUIRE_E2E=1 to make missing fixtures a failure (CI does).
 */

const requireEnv = process.env.CERBERUS_REQUIRE_E2E === "1";

function resolveFixture(
  envVar: string,
  ...fallback: string[]
): string {
  const fromEnv = process.env[envVar];
  if (fromEnv) return fromEnv;
  const here = dirname(fileURLToPath(import.meta.url));
  return resolve(here, "..", ...fallback);
}

const cerberus = resolveFixture(
  "CERBERUS_BIN",
  "..",
  "..",
  "CerberusMCP",
  "target",
  "debug",
  "cerberus",
);
const upstream = resolveFixture(
  "CERBERUS_FAKE_UPSTREAM_BIN",
  "..",
  "rs",
  "target",
  "debug",
  "examples",
  "fake_upstream",
);
const haveFixtures = existsSync(cerberus) && existsSync(upstream);

if (!haveFixtures && requireEnv) {
  throw new Error(
    `CERBERUS_REQUIRE_E2E=1 but fixtures are missing: ${cerberus}, ${upstream}`,
  );
}

function registry(upstreamPath: string): string {
  // TOML basic strings: escape backslashes (Windows paths).
  const up = upstreamPath.replaceAll("\\", "\\\\");
  return `schema_version = 2

[settings]
default_mode = "direct"

[servers.echo-server]
description = "e2e echo upstream"
timeout_ms = 15000

[servers.echo-server.transport]
type = "stdio"
command = "${up}"
args = []
env = { CERBERUS_FAKE_UPSTREAM = { source = "literal", value = "1" } }

[servers.echo-server.policy]
deny_tools = []

[servers.locked]
description = "e2e upstream whose echo tool is denied"
timeout_ms = 15000

[servers.locked.transport]
type = "stdio"
command = "${up}"
args = []

[servers.locked.policy]
deny_tools = ["echo"]
`;
}

const suite = haveFixtures ? describe : describe.skip;
const configDirs: string[] = [];
const children: ChildProcess[] = [];

const HTTP_TOKEN = "e2e-http-secret";

/** Spawn `cerberus gateway start --transport http` and resolve its URL. */
async function spawnHttpGateway(config: string): Promise<string> {
  const child = spawn(
    cerberus,
    ["gateway", "start", "--transport", "http", "--bind", "127.0.0.1:0", "--token", HTTP_TOKEN],
    {
      env: {
        ...process.env,
        CERBERUS_CONFIG_DIR: config,
        CERBERUS_DATA_DIR: join(config, "data"),
      },
      stdio: "ignore",
    },
  );
  children.push(child);

  // The state file names the resolved bind (port 0 -> real port) and lands
  // just before the server starts accepting.
  const stateFile = join(config, "data", "state", "gateway.json");
  const deadline = Date.now() + 15_000;
  for (;;) {
    try {
      const state = JSON.parse(readFileSync(stateFile, "utf8")) as {
        bind?: string;
      };
      if (state.bind && !state.bind.endsWith(":0")) {
        return `http://${state.bind}/mcp`;
      }
    } catch {
      /* not written yet */
    }
    if (Date.now() > deadline) {
      throw new Error(`gateway state file never reported a port: ${stateFile}`);
    }
    await new Promise((r) => setTimeout(r, 50));
  }
}

suite("gateway stdio e2e", () => {
  it("lists routed tools, round-trips a call, and shapes policy_denied", async () => {
    const config = mkdtempSync(join(tmpdir(), "cerberus-sdk-e2e-"));
    configDirs.push(config);
    writeFileSync(join(config, "conf.cerberus"), registry(upstream));

    const gw = await Gateway.connectStdio(cerberus, {
      env: {
        CERBERUS_CONFIG_DIR: config,
        CERBERUS_DATA_DIR: join(config, "data"),
      },
    });
    try {
      const tools = await gw.listTools();
      const echo = tools.find((t) => t.name === "echo-server__echo");
      expect(echo).toMatchObject({ server: "echo-server", tool: "echo" });
      expect(tools.some((t) => t.name.startsWith("locked__"))).toBe(false);

      const out = await gw.call("echo-server", "echo", { text: "hi" });
      expect(out.text).toBe("echo: hi");

      const err = await gw.call("locked", "echo", { text: "hi" }).then(
        () => undefined,
        (e: unknown) => e,
      );
      expect(err).toBeInstanceOf(GatewayError);
      expect(err).toMatchObject({
        code: "policy_denied",
        retryable: false,
        exitCode: 6,
        server: "locked",
        tool: "echo",
      });
    } finally {
      await gw.close();
    }
  });
});

suite("gateway http e2e", () => {
  it("enforces bearer auth and serves the routed contract", async () => {
    const config = mkdtempSync(join(tmpdir(), "cerberus-sdk-e2e-http-"));
    configDirs.push(config);
    writeFileSync(join(config, "conf.cerberus"), registry(upstream));

    const url = await spawnHttpGateway(config);

    // Missing and wrong tokens are rejected at the HTTP layer.
    await expect(Gateway.connectHttp(url)).rejects.toThrow();
    await expect(
      Gateway.connectHttp(url, { bearer: "wrong-secret" }),
    ).rejects.toThrow();

    // The real token gets the full contract.
    const gw = await Gateway.connectHttp(url, { bearer: HTTP_TOKEN });
    try {
      const tools = await gw.listTools();
      const echo = tools.find((t) => t.name === "echo-server__echo");
      expect(echo).toMatchObject({ server: "echo-server", tool: "echo" });
      expect(tools.some((t) => t.name.startsWith("locked__"))).toBe(false);

      const out = await gw.call("echo-server", "echo", { text: "hi" });
      expect(out.text).toBe("echo: hi");

      const err = await gw.call("locked", "echo", { text: "hi" }).then(
        () => undefined,
        (e: unknown) => e,
      );
      expect(err).toBeInstanceOf(GatewayError);
      expect(err).toMatchObject({
        code: "policy_denied",
        retryable: false,
        exitCode: 6,
        server: "locked",
        tool: "echo",
      });
    } finally {
      await gw.close();
    }
  });
});

afterAll(() => {
  for (const child of children) {
    child.kill();
  }
  for (const dir of configDirs) {
    rmSync(dir, { recursive: true, force: true });
  }
});
