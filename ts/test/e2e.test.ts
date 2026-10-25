import { existsSync, mkdtempSync, rmSync, writeFileSync } from "node:fs";
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
  return `schema_version: 1

settings:
  default_mode: direct

servers:
  echo-server:
    description: e2e echo upstream
    transport:
      type: stdio
      command: '${upstreamPath}'
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
      command: '${upstreamPath}'
      args: []
    timeout_ms: 15000
    policy:
      deny_tools: ["echo"]
`;
}

const suite = haveFixtures ? describe : describe.skip;
const configDirs: string[] = [];

suite("gateway stdio e2e", () => {
  it("lists routed tools, round-trips a call, and shapes policy_denied", async () => {
    const config = mkdtempSync(join(tmpdir(), "cerberus-sdk-e2e-"));
    configDirs.push(config);
    writeFileSync(join(config, "cerberus.yaml"), registry(upstream));

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

afterAll(() => {
  for (const dir of configDirs) {
    rmSync(dir, { recursive: true, force: true });
  }
});
