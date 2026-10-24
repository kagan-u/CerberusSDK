/**
 * TypeScript/JavaScript SDK for the CerberusMCP gateway.
 *
 * Wraps the official `@modelcontextprotocol/client` with two Cerberus
 * additions on top of plain MCP:
 *
 * - `RoutedTool`: every listed tool resolves its upstream `server`/`tool`
 *   from `_meta["cerberus.dev/route"]` — no `{server}__{tool}` splitting.
 * - `GatewayError`: failing calls throw a typed error carrying the gateway
 *   `code`, `retryable`, and the CLI-aligned `exitCode`.
 *
 * Wire contract: `../docs/contract.md`.
 */

import {
  Client,
  StreamableHTTPClientTransport,
  type Implementation,
  type JSONValue,
  type Tool,
  type Transport,
} from "@modelcontextprotocol/client";
import { StdioClientTransport } from "@modelcontextprotocol/client/stdio";

/** Reverse-DNS `_meta` key carrying `{server, tool}` on every listed tool. */
export const ROUTE_META_KEY = "cerberus.dev/route";

const DEFAULT_CONNECT_TIMEOUT_MS = 30_000;

// ---------------------------------------------------------------------------
// Error codes
// ---------------------------------------------------------------------------

/** Stable gateway error codes (snake_case on the wire). */
export const GATEWAY_ERROR_CODES = [
  "unknown_tool",
  "server_not_found",
  "policy_denied",
  "confirmation_required",
  "upstream_unavailable",
  "shutting_down",
  "concurrency_saturated",
  "timeout",
  "upstream_error",
] as const;

export type GatewayErrorCode = (typeof GATEWAY_ERROR_CODES)[number];

/** Whether an SDK may automatically retry a call that failed this way. */
export function isRetryable(code: GatewayErrorCode): boolean {
  return (
    code === "upstream_unavailable" ||
    code === "shutting_down" ||
    code === "concurrency_saturated"
  );
}

/** Process exit code the `cerberus` CLI reports for the same condition. */
export function exitCode(code: GatewayErrorCode): number {
  switch (code) {
    case "unknown_tool":
    case "server_not_found":
      return 5;
    case "policy_denied":
    case "confirmation_required":
      return 6;
    case "timeout":
    case "concurrency_saturated":
      return 7;
    default:
      return 1;
  }
}

function isGatewayErrorCode(value: unknown): value is GatewayErrorCode {
  return (
    typeof value === "string" &&
    (GATEWAY_ERROR_CODES as readonly string[]).includes(value)
  );
}

/** A gateway-reported tool failure (from an `isError: true` result). */
export class GatewayError extends Error {
  readonly code: GatewayErrorCode;
  readonly retryable: boolean;
  readonly server?: string;
  readonly tool?: string;
  readonly reason?: string;
  /** Process exit code for the same condition, from the contract. */
  readonly exitCode: number;

  constructor(init: {
    code: GatewayErrorCode;
    message: string;
    retryable?: boolean;
    server?: string;
    tool?: string;
    reason?: string;
  }) {
    super(init.message);
    this.name = "GatewayError";
    this.code = init.code;
    this.retryable = init.retryable ?? isRetryable(init.code);
    this.server = init.server;
    this.tool = init.tool;
    this.reason = init.reason;
    this.exitCode = exitCode(init.code);
  }
}

// ---------------------------------------------------------------------------
// Tool routing
// ---------------------------------------------------------------------------

/** A tool as listed by the gateway, with its upstream coordinates resolved. */
export interface RoutedTool {
  /** Public name for `tools/call` (`{server}__{tool}`). */
  name: string;
  server: string;
  /** The tool's own name on its upstream server. */
  tool: string;
  description?: string;
  inputSchema: unknown;
}

/** Public name for an upstream tool. */
export function publicToolName(server: string, tool: string): string {
  return `${server}__${tool}`;
}

type RouteMeta = { server?: JSONValue; tool?: JSONValue };

function routeFromTool(tool: Tool): { server: string; tool: string } | undefined {
  const meta = (tool as { _meta?: Record<string, unknown> })._meta;
  const route = meta?.[ROUTE_META_KEY] as RouteMeta | undefined;
  if (route && typeof route.server === "string" && typeof route.tool === "string") {
    return { server: route.server, tool: route.tool };
  }
  const name = tool.name;
  const sep = name.indexOf("__");
  if (sep <= 0 || sep === name.length - 2) return undefined;
  return { server: name.slice(0, sep), tool: name.slice(sep + 2) };
}

/**
 * Resolves a listed tool's upstream coordinates. Prefers
 * `_meta["cerberus.dev/route"]`; falls back to first-`__` name parsing for
 * pre-contract servers. Throws when neither is possible.
 */
export function routedTool(tool: Tool): RoutedTool {
  const route = routeFromTool(tool);
  if (!route) {
    throw new Error(
      `tool \`${tool.name}\` has no route metadata and no \`__\` separator`,
    );
  }
  return {
    name: tool.name,
    server: route.server,
    tool: route.tool,
    ...(tool.description !== undefined
      ? { description: tool.description }
      : {}),
    inputSchema: tool.inputSchema,
  };
}

// ---------------------------------------------------------------------------
// Result parsing
// ---------------------------------------------------------------------------

/** A successful tool result: structured payload when present, text always. */
export interface ToolOutput {
  structured?: JSONValue;
  text: string;
}

function textOf(content: ReadonlyArray<{ type: string; text?: string }>): string {
  return content
    .filter((block) => block.type === "text" && typeof block.text === "string")
    .map((block) => block.text as string)
    .join("\n");
}

/**
 * Builds a `GatewayError` from an `isError: true` result, preferring
 * `structuredContent` and degrading to `upstream_error` without it.
 */
export function gatewayError(
  result: {
    content: ReadonlyArray<{ type: string; text?: string }>;
    structuredContent?: unknown;
  },
  fallbackServer: string,
  fallbackTool: string,
): GatewayError {
  const text = textOf(result.content);
  const sc =
    result.structuredContent && typeof result.structuredContent === "object"
      ? (result.structuredContent as Record<string, unknown>)
      : undefined;
  if (!sc) {
    return new GatewayError({
      code: "upstream_error",
      message: text || "tool call failed",
      server: fallbackServer,
      tool: fallbackTool,
    });
  }
  const code: GatewayErrorCode = isGatewayErrorCode(sc.code)
    ? sc.code
    : "upstream_error";
  const message =
    typeof sc.message === "string" && sc.message.length > 0
      ? sc.message
      : text || code;
  const out: ConstructorParameters<typeof GatewayError>[0] = {
    code,
    message,
    ...(typeof sc.retryable === "boolean"
      ? { retryable: sc.retryable }
      : {}),
    ...(typeof sc.server === "string"
      ? { server: sc.server }
      : fallbackServer
        ? { server: fallbackServer }
        : {}),
    ...(typeof sc.tool === "string"
      ? { tool: sc.tool }
      : fallbackTool
        ? { tool: fallbackTool }
        : {}),
    ...(typeof sc.reason === "string" ? { reason: sc.reason } : {}),
  };
  return new GatewayError(out);
}

// ---------------------------------------------------------------------------
// Gateway connection
// ---------------------------------------------------------------------------

export interface GatewayConnectOptions {
  /** Client identity announced during `initialize`. */
  clientInfo?: Implementation;
  /** Extra CLI args for `cerberus gateway start` (stdio only). */
  args?: string[];
  /** Environment for the spawned process (stdio only; defaults to inherit). */
  env?: Record<string, string>;
}

/** A connection to a Cerberus gateway (stdio or streamable HTTP). */
export class Gateway {
  private constructor(private readonly client: Client) {}

  private static async fromTransport(
    transport: Transport,
    clientInfo?: Implementation,
  ): Promise<Gateway> {
    const client = new Client(clientInfo ?? {
      name: "cerberus-sdk",
      version: "0.1.0",
    });
    let timer: ReturnType<typeof setTimeout> | undefined;
    const timeout = new Promise<never>((_, reject) => {
      timer = setTimeout(
        () =>
          reject(
            new Error(
              `gateway connect timed out after ${DEFAULT_CONNECT_TIMEOUT_MS}ms`,
            ),
          ),
        DEFAULT_CONNECT_TIMEOUT_MS,
      );
    });
    try {
      await Promise.race([client.connect(transport), timeout]);
    } catch (error) {
      await client.close().catch(() => undefined);
      throw error;
    } finally {
      if (timer) clearTimeout(timer);
    }
    return new Gateway(client);
  }

  /**
   * Spawn `cerberus gateway start --transport stdio` and speak MCP over it.
   */
  static async connectStdio(
    command = "cerberus",
    options: GatewayConnectOptions = {},
  ): Promise<Gateway> {
    const transport = new StdioClientTransport({
      command,
      args: [
        "gateway",
        "start",
        "--transport",
        "stdio",
        ...(options.args ?? []),
      ],
      ...(options.env ? { env: options.env } : {}),
      stderr: "pipe",
    });
    return Gateway.fromTransport(transport, options.clientInfo);
  }

  /** Connect to a streamable-HTTP gateway endpoint. */
  static async connectHttp(
    url: string | URL,
    options: { bearer?: string; clientInfo?: Implementation } = {},
  ): Promise<Gateway> {
    const transport = new StreamableHTTPClientTransport(new URL(url), {
      ...(options.bearer
        ? {
            requestInit: {
              headers: { authorization: `Bearer ${options.bearer}` },
            },
          }
        : {}),
    });
    return Gateway.fromTransport(transport, options.clientInfo);
  }

  /** The underlying official client, for APIs this SDK does not wrap yet. */
  get underlying(): Client {
    return this.client;
  }

  /** List tools with upstream coordinates resolved from `_meta`. */
  async listTools(): Promise<RoutedTool[]> {
    const result = await this.client.listTools();
    return result.tools.map(routedTool);
  }

  /** Call `tool` on upstream `server` through the gateway. */
  async call(
    server: string,
    tool: string,
    args: Record<string, unknown> = {},
  ): Promise<ToolOutput> {
    const result = await this.client.callTool({
      name: publicToolName(server, tool),
      arguments: args,
    });
    if (result.isError === true) {
      throw gatewayError(result, server, tool);
    }
    return {
      ...(result.structuredContent !== undefined
        ? { structured: result.structuredContent as JSONValue }
        : {}),
      text: textOf(
        result.content as ReadonlyArray<{ type: string; text?: string }>,
      ),
    };
  }

  /** Close the connection (and any spawned gateway process). */
  async close(): Promise<void> {
    await this.client.close();
  }
}
