# cerberus-sdk (Python)

Python SDK for the CerberusMCP gateway, wrapping the official
[`mcp`](https://pypi.org/project/mcp/) client.

```python
from cerberus_sdk import Gateway

async with await Gateway.connect_stdio() as gw:
    for t in await gw.list_tools():
        print(t.name, "->", t.server, "::", t.tool)
    out = await gw.call("github", "search_repos", {"q": "mcp"})
    print(out.text, out.structured)
```

Failing calls raise `GatewayError` with the contract's `code`, `retryable`,
and `exit_code` (see `../docs/contract.md`).
