import { describe, expect, it } from "vitest";
import type { Tool } from "@modelcontextprotocol/client";
import {
  GatewayError,
  GatewayErrorCode,
  GATEWAY_ERROR_CODES,
  ROUTE_META_KEY,
  exitCode,
  gatewayError,
  isRetryable,
  publicToolName,
  routedTool,
} from "../src/index.js";

function fakeTool(name: string, route?: { server: string; tool: string }): Tool {
  const base = {
    name,
    description: "desc",
    inputSchema: { type: "object", properties: {} },
  } as unknown as Tool & { _meta?: Record<string, unknown> };
  if (route) {
    base._meta = {
      [ROUTE_META_KEY]: { server: route.server, tool: route.tool },
    };
  }
  return base as Tool;
}

describe("routedTool", () => {
  it("prefers route metadata over the public name", () => {
    const routed = routedTool(
      fakeTool("fs__read_file", {
        server: "remote-filesystem",
        tool: "read_file",
      }),
    );
    expect(routed.server).toBe("remote-filesystem");
    expect(routed.tool).toBe("read_file");
    expect(routed.name).toBe("fs__read_file");
    expect(publicToolName(routed.server, routed.tool)).toBe(
      "remote-filesystem__read_file",
    );
  });

  it("falls back to first-__ name parsing without metadata", () => {
    const routed = routedTool(fakeTool("srv__do__it"));
    expect(routed.server).toBe("srv");
    expect(routed.tool).toBe("do__it");
  });

  it("rejects tools with neither metadata nor separator", () => {
    expect(() => routedTool(fakeTool("plain_tool"))).toThrow(
      /no route metadata/,
    );
  });
});

describe("gatewayError", () => {
  it("parses the structured error envelope", () => {
    const err = gatewayError(
      {
        content: [{ type: "text", text: "policy denied: deny pattern matched" }],
        structuredContent: {
          code: "policy_denied",
          message: "policy denied: deny pattern matched",
          retryable: false,
          exit_code: 6,
          server: "github",
          tool: "search_repos",
          reason: "deny pattern matched",
        },
      },
      "github",
      "search_repos",
    );
    expect(err).toBeInstanceOf(GatewayError);
    expect(err.code).toBe("policy_denied");
    expect(err.retryable).toBe(false);
    expect(err.exitCode).toBe(6);
    expect(err.server).toBe("github");
    expect(err.tool).toBe("search_repos");
    expect(err.reason).toBe("deny pattern matched");
    expect(err.message).toContain("policy denied");
  });

  it("degrades to upstream_error without structured content", () => {
    const err = gatewayError(
      { content: [{ type: "text", text: "boom" }] },
      "fs",
      "read",
    );
    expect(err.code).toBe("upstream_error");
    expect(err.retryable).toBe(false);
    expect(err.exitCode).toBe(1);
    expect(err.server).toBe("fs");
    expect(err.tool).toBe("read");
    expect(err.message).toBe("boom");
  });

  it("treats unknown codes as upstream_error but keeps the message", () => {
    const err = gatewayError(
      {
        content: [],
        structuredContent: { code: "brand_new_code", message: "hm" },
      },
      "s",
      "t",
    );
    expect(err.code).toBe("upstream_error");
    expect(err.message).toBe("hm");
  });
});

describe("error code contract", () => {
  it("matches the contract table", () => {
    const table: Array<[GatewayErrorCode, boolean, number]> = [
      ["unknown_tool", false, 5],
      ["server_not_found", false, 5],
      ["policy_denied", false, 6],
      ["confirmation_required", false, 6],
      ["timeout", false, 7],
      ["concurrency_saturated", true, 7],
      ["upstream_unavailable", true, 1],
      ["shutting_down", true, 1],
      ["upstream_error", false, 1],
    ];
    expect(GATEWAY_ERROR_CODES).toHaveLength(table.length);
    for (const [code, retryable, exit] of table) {
      expect(isRetryable(code)).toBe(retryable);
      expect(exitCode(code)).toBe(exit);
    }
  });
});
