# Scripting and agents

The Python client and MCP server use the terminal's HTTP API. They trade a paper
simulator on the data openportd has, often delayed. They cannot send live orders.
Scenario and demo prices are generated, and are labelled simulated. Account money
is decimal text; use `decimal.Decimal` for calculations.

The client supports Python 3.10+ and has no runtime dependencies. Install from a
checkout with `python -m pip install ./python`. Optional extras are `./python[pandas]`
for frames, `./python[ws]` for WebSockets, and `./python[mcp]` for the MCP server
(`mcp>=2.2,<3`). A source checkout also works with `PYTHONPATH=python/src`.

```python
from openport import Client, ApiError

client = Client("http://127.0.0.1:8080", token="YOUR_TOKEN", account="practice", timeout=30)
print(client.status())                 # provider, delay and per-symbol market times
print(client.volatility("SPX"))        # includes missing-value reasons
print(client.portfolio())
print(client.preview_order(symbol="SPXW  261022P05000000", side="buy",
    type="limit", time_in_force="day", quantity=1, limit_price="0.05"))
```

Discover current contract symbols with `chain()` before submitting an order; the
symbol above is an example. `place_order` and `preview_order` generate a UUID
`client_order_id` if omitted. The client retries HTTP 503 up to three times with
0.1, 0.2 and 0.4 second delays, reusing the same body and ID. Other HTTP failures
raise `ApiError`, carrying `status`, `code` / `reason_code`, and the server's
`actual`, `limit` and `scope`. Transport failures are not retried. If retrying an
order after an uncertain response, supply the original ID and identical terms.
Other writes do not have the order endpoint's idempotency guarantee.

Results are dictionaries annotated by small `TypedDict`s in `openport.types`.
Unknown fields are preserved. The client never replaces missing quotes or P&L
with zero. It does not coerce monetary strings into floats.

| Methods | HTTP routes |
| --- | --- |
| `status`, `symbols` | `/api/status`; symbols are its underlying names |
| `summary`, `series` | `/api/underlyings/{symbol}/summary`; series are its expiries |
| `chain`, `exposure`, `surface`, `volatility`, `candles` | The corresponding underlying views |
| `account`, `portfolio`, `orders`, `fills`, `trades`, `risk`, `plans`, `accounts`, `equity` | The terminal's paper reads |
| `place_order`, `preview_order`, `modify_order`, `cancel_order`, `cancel_all`, `flatten` | Paper orders and position closure |
| `note`, `day_note`, `export_csv` | Trade/day annotations and fills/trades exports |
| `create_account`, `reset_account`, `payout`, `limits`, `guardrails`, `kill`, `settle`, `exercise`, `close_stock` | Remaining terminal commands |
| `list_replays`, `start_replay`, `control_replay`, `step_replay(until)`, `stop_replay`, `delete_replay` | Replay controls and history deletion |

`client.for_replay("main")` reads and trades the active replay's isolated account.
`client.for_history(run_id)` reads a finished run. Replay controls always address
`/api/replay` and never receive the client's `account` query parameter. Historical
runs retain accounts and trades, not complete market analytics.

```python
from openport import chain_frame, smile_frame, trades_frame, ticks

chain_df = chain_frame(client.chain("SPX"))       # one row per strike and side
smile_df = smile_frame(client.surface("SPX"))    # one row per expiry and smile point
trades_df = trades_frame(client.trades())        # option round trips; money stays text

async def watch():
    async for tick in ticks("ws://127.0.0.1:8080/ws", token="YOUR_TOKEN"):
        print(tick)
```

Frame helpers do not add or estimate missing values. The WebSocket iterator yields
raw tick dictionaries and closes with its async context; it does not reconnect or
reconstruct messages missed during a disconnect.

## MCP clients

Set `OPENPORT_WRITE_TOKEN` in the MCP process's environment. Use a named token with
`read,trade:practice,replay` for an agent that may read, trade `practice` and run
replays. Use `--require-token` on openportd if reads must also be authenticated.
Without a token the MCP tools can read a public instance, but cannot write even if
the server permits unauthenticated loopback writes.

After installing `./python[mcp]`, add it to Claude Code:

```sh
claude mcp add openport -- python -m openport_mcp --url http://127.0.0.1:8080
```

Ensure the launched process inherits `OPENPORT_WRITE_TOKEN`. Other MCP clients
can launch the same stdio process:

```json
{
  "mcpServers": {
    "openport": {
      "command": "python",
      "args": ["-m", "openport_mcp", "--url", "http://127.0.0.1:8080", "--agent-name", "research"],
      "env": {"OPENPORT_WRITE_TOKEN": "YOUR_NAMED_TOKEN_SECRET"}
    }
  }
}
```

Store the actual secret in your client's protected environment configuration. It
is never a tool argument. The installed SDK is MCP 2.x: `MCPServer`, not `FastMCP`.

Tools include status, symbols, summary, chain, volatility, exposure, account,
positions, orders, risk, order preview/place/cancel, flatten, scenario listing and
replay start/step/stop (scenarios or recordings). Chain results default to a 3% strike window and at most 80
strikes; a capped response says `truncated: true`. Windows must be in `(0, 0.1]`.

Every tool result includes `as_of` from market data, `feed` with provider and delay,
and `simulated`. Unavailable source metadata is null. Source timestamps precede a
command unless its response contains a newer market timestamp; replay controls
refresh the replay's published quote times. Replay receipt clocks are not used as
market timestamps. Scenario entries
are also explicitly labelled simulated. Errors retain reason codes. Writes name
an account and return `paper: true`; replay writes must name `main`, the isolated
replay account. Placed and previewed orders carry `agent:NAME`. The authenticated
token's name is the journal actor; an agent tag is descriptive, not authentication.

The MCP server defaults to stdio. It delegates scope enforcement to openportd and
does not have any broker or live-trading connection.

## Examples and checks

`examples/volatility.py` prints SPX's 30-day model-free IV and skew.
`examples/put_spread.py --account practice` discovers quoted puts, previews a
vertical and places a paper limit with a bracket. `examples/last_hour.py SCENARIO`
starts a paused simulated day, steps to 15:00 New York time, places that spread,
steps to 15:59 and flattens. They use `OPENPORT_URL` and `OPENPORT_WRITE_TOKEN`.
The last-hour example requires an SPX scenario that spans those times. Quotes,
liquidity, the plan and session can reject these examples; no result is promised.

From the repository root, using the prepared environment:

```sh
.venv-py/bin/python -m pytest python/tests
.venv-py/bin/python tools/contract_test.py http://127.0.0.1:8080 --token "$OPENPORT_WRITE_TOKEN" --spec docs/openapi.yaml
```

Tests use a threaded `http.server` with synthetic JSON. When a sandbox denies TCP
binding, the same HTTP handler runs over a local socket pair. If the C++ test
binary is built, Python also validates real handler responses captured without a
listener. The contract command checks every API GET and a preview, resting limit
and cancellation in a new simulated replay. It refuses to replace an active run,
requires durable replay history and leaves the completed test run in history.
Static files and WebSocket transport are described in OpenAPI; the HTTP contract
command checks the JSON API and CSV routes. The first mismatch prints its route
and field and exits nonzero. CI runs it after the Docker smoke test.
