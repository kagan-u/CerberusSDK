# Cerberus gateway wire contract

Source of truth: `CerberusMCP/docs/gateway.md`. Every SDK in this repository
implements exactly this contract; the `cerberus` binary produces it.

## 1. Tool routing (`tools/list`)

Tools are published as `{server}__{tool}` (server names never contain `__`).
Every tool entry additionally carries:

```json
"_meta": {
  "cerberus.dev/route": { "server": "remote-filesystem", "tool": "read_file" }
}
```

- `cerberus.dev/route.server` / `.tool` are **authoritative**.
- Name parsing (`split_once("__")`, server part first) is only a fallback for
  servers older than this contract.
- The nested reverse-DNS key avoids collisions with other `_meta` producers
  (W3C trace context keys, host tooling).

## 2. Structured tool errors (`tools/call`)

A failing call returns `isError: true` with a human message in `content` and
machine-readable data in `structuredContent`:

```json
{
  "isError": true,
  "content": [{ "type": "text", "text": "policy denied: ..." }],
  "structuredContent": {
    "code": "policy_denied",
    "message": "policy denied: ...",
    "retryable": false,
    "exit_code": 6,
    "server": "github",
    "tool": "search_repos",
    "reason": "deny pattern matched"
  }
}
```

`server`, `tool`, `reason` are present only when known. `code` values
(stable, snake_case) and their CLI-aligned `exit_code`:

| code | retryable | exit_code |
| --- | --- | --- |
| `unknown_tool` | no | 5 |
| `server_not_found` | no | 5 |
| `policy_denied` | no | 6 |
| `confirmation_required` | no | 6 |
| `timeout` | no | 7 |
| `concurrency_saturated` | no | 7 |
| `upstream_unavailable` | yes | 1 |
| `shutting_down` | yes | 1 |
| `upstream_error` | no | 1 |

Envelope keys are camelCase (`isError`, `structuredContent`, `resultType`);
an error result without `structuredContent` means the failure predates the
contract and must be treated as `upstream_error` (not retryable).

## 3. Connecting

- stdio: spawn `cerberus gateway start --transport stdio` (add `--profile`,
  and point `CERBERUS_CONFIG_DIR`/`CERBERUS_DATA_DIR` at the config to use).
- HTTP: `POST` a streamable-HTTP endpoint; remote binds require
  `--allow-remote` and a bearer token (`Authorization: Bearer ...`).
