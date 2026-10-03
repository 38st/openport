# API and scripting

The HTTP routes the web terminal uses, by area. The [OpenAPI contract](openapi.yaml)
is the full reference, and [paper trading](paper-trading.md) documents every order
field, rule and reason code.

Money is exact decimal text, never a JSON number. Strings preserve every meaningful
micro-dollar with 2 to 6 decimal places, omitting trailing zeros beyond cents.
Computed requirements, buying power under portfolio margin and prorated values can
carry more than cents: for example, available buying power may be `"2033.725875"`
beside cash of `"100000.00"`. Clients should parse money as exact decimals and round
only for display (the terminal shows that buying power as `$2,033.73`). Booked values
remain exact integer micro-dollars.

## Status and market data

| Route | Returns |
| --- | --- |
| `GET /api/status` | Running `version`, provider, market and per-underlying sessions, feed health (including the demo day title), engine counters, notification delivery status (no secrets), and optional sandbox availability |
| `POST /api/notifications/test` | Queue a test for `{ "channel": "ID" }`; requires admin |
| `PUT /api/notifications/channels/ID` | Change a channel's enabled state, event filters and floor distance for this process; requires admin |
| `GET /api/underlyings/{symbol}/summary` | Spot and its source, expiries with forward, rate and its source, ATM IV, GEX, VEX, coverage and `last_trade` / `auto_close` UTC ISO times |
| `GET /api/underlyings/{symbol}/chain?expiry={id}` | Every strike with both sides' quotes, IV, Greeks, early-exercise premium, `volume` (session contracts or null) and whether paper orders can fill on each quote (`executable`, `quote_issue`); coverage includes `volume` |
| `GET /api/underlyings/{symbol}/exposure?expiries=8` | GEX and VEX by strike and expiry, flip and walls |
| `GET /api/underlyings/{symbol}/surface?expiries=12` | Smile points per expiry |
| `GET /api/underlyings/{symbol}/volatility` | Current model-free IV, ATM/skew, realized vol, cones, implied moves, IV rank/percentile and ex-ante/ex-post VRP, with sources and history counts |
| `GET /api/underlyings/{symbol}/series?fields=mfiv30,atm30,rr25&from=&to=&interval=1d` | Selected metric history at `1m` or `1d`; bounded date ranges, null gaps and per-value sources ([fields and limits](volatility.md#series-api)) |
| `GET /api/underlyings/{symbol}/candles?interval=5m` | OHLC bars at 1m, 5m, 15m, 30m, 1h or 1d, oldest first |
| `WS /ws` | A small tick each second with versions, so clients refetch only what changed; at most 256 open sessions, after which an upgrade gets 503 with `Retry-After: 5` |

Expiry ids are the date plus settlement, for example `2026-10-16AM`.

## Paper trading

With paper trading on, the same API is the account.

Create/reset accepts optional `fee_model: "flat" | "itemized"` or custom
`rules.fees` with open/close commission, per-leg cap, clearing, regulatory,
root-specific index fees and per-contract exercise fees. The preset overrides
custom fees. Fills and order/change previews return itemized `fees` (null under
flat fees); preview `fee` is the full remaining size’s total, null without a
projection. See [fee semantics and bounds](paper-trading.md#fees).

Example schedule for custom rules (omitted fields default to zero):

```json
{"fees":{"open":"1.00","close":"0.00","leg_cap":"10.00",
         "clearing":"0.10","regulatory":"0.02",
         "index":{"SPX":"0.60","SPXW":"0.60"},"exercise":"5.00"}}
```

The web terminal uses these routes, so anything it does can be scripted:

| Route | Does |
| --- | --- |
| `GET /api/portfolio`, `/api/orders`, `/api/fills`, `/api/risk`, `/api/account`, `/api/trades` | The account's positions with their margin breakdown, orders and fills with actors (each fill with the bid, ask and sizes it took and the quote's age), risk, warnings and breach estimates, rules and progress, and its round trips |
| `POST /api/orders/preview` | A pure order check, buying power, Greeks change, maximum loss, size to buying power and to floor, projected breach risk, and warnings about terms that act at once |
| `POST /api/orders/what-if` | Up to six candidate adjustments of up to four orders each, filled on a private copy of the account and compared on buying power, Greeks, grid loss and floor room |
| `POST /api/orders/{id}/preview` | The same preview of a change to a resting order, as `PUT /api/orders/{id}` would make it, without making it |
| `POST /api/stocks/trade` | `{symbol, side: "buy" or "sell", shares: 1–10000000}` opens, adds, reduces or reverses shares at a fresh underlying price during stock regular hours; returns Portfolio |
| `POST /api/stocks/trade/preview` | Same body; returns `decision` (`ok` or reason code), `reason`, price, signed cash cost, and current/after buying power and dollar delta without recording a trade |
| `POST /api/stocks/close` | `{symbol, shares?}` reduces only, all held shares if omitted; an oversized close is refused |
| `POST /api/positions/close/preview` | A flatten's dry run: what it would cancel and close now, and the account after it, without doing it |
| `GET/POST /api/playbooks`, `GET/PUT/DELETE /api/playbooks/{id}` | Versioned definitions, archive, account stages and adherence/expectancy reports; `?version=N` reads an old version |
| `PUT /api/playbooks/{id}/mode`, `POST /api/playbooks/staged/{stage}/send`, `/dismiss` | Enable staging or replay-only auto; send or dismiss a current stage |
| `GET/POST/DELETE /api/backtests`, `GET/DELETE /api/backtests/{id}` | Start one batch job, watch progress, read kept reports or cancel; mutations need replay scope |
| `GET /api/account/pass-odds?days=N&samples=M&playbook=ID&seed=S` | Seeded estimate from historical equity days, not a prediction; needs ten days with intraday extremes (422 `PASS_ODDS_UNAVAILABLE` otherwise) |
| `GET /api/strategy-template?symbol=SPX&expiry=ID&template=JSON` | Shared server leg selection for terminal templates and playbooks |
| `GET /api/account/equity?from=&to=` | Persisted minute and fill equity, floor, high-water mark and target; optional UTC ISO time bounds |
| `POST /api/orders`, `PUT /api/orders/{id}`, `DELETE /api/orders/{id}` | Place an order (one contract, or `legs` for a strategy), attach held-spread exits with `exits_only`, change it or cancel it |
| `POST /api/orders/cancel`, `POST /api/positions/close` | Cancel every open order, or flatten, for one underlying or all |
| `POST /api/positions/abandon` | Give up a long at zero without a fee: `{symbol}`, allowed with a fresh ask-only quote or after expiry |
| `POST /api/positions/instruction` | Set or withdraw a long's expiry instruction: `{symbol, do_not_exercise}`; true forfeits shares and cash settlement. Both routes return the portfolio and require trade scope (replay scope on the replay mirror) |
| `GET /api/trades.csv`, `/api/fills.csv` | Trades or fills, with context and excursions, filtered by account and New York `from`/`to` dates. Fills include `fees.commission`, `fees.clearing`, `fees.regulatory`, and `fees.index` columns (empty for flat fees) |
| `PUT /api/days/{YYYY-MM-DD}/note` | The account's plan and review for a day; returned in `/api/trades` as `day_notes` |
| `PUT /api/trades/{id}/note` | A trade's note and tags, or a share trade's (`s1`, ...) |
| `POST /api/trades/group`, `/api/trades/ungroup` | Join open round trips' trades into one whole trade, or take a round trip out of its trade |
| `PUT /api/risk/limits`, `PUT /api/risk/guardrails`, `POST /api/risk/kill` | Tighten rules now or queue looser values for rollover; set personal guardrails; trip or reset the kill switch |
| `GET /api/plans`, `POST /api/account/reset` | The plans, and a new attempt; optional `fill_model` selects `as_displayed` or `conservative`, and optional `margin`, `account_type` (`margin`, `cash`, `ira`), `house_margin_percent` and `pm_vol_shock` set the account's margin |
| `POST /api/sandboxes` | Create a private demo practice account and return its token once; unauthenticated when enabled, 404 when off, 429 at capacity or a creation rate limit |
| `GET /api/accounts`, `POST /api/accounts` | List the accounts or create one, with optional `fill_model` and margin settings as for a reset; account routes take `?account=ID` for one other than the main account |
| `GET`, `POST`, `PUT`, `DELETE /api/replay` | List recordings, scenarios and run history; start `{file}` or `{scenario}` (`demo` also accepted), with `plan`, `speed`, `start_at`, `paused` and scenario `seed`/`date`, or continue an interrupted saved run with `{resume}`; control or stop. `/api/replay/X` mirrors `/api/X` |
| `PUT /api/replay {"until":"HH:MM[:SS]"}` | Advance through a New York session time (its next occurrence in a scenario of several sessions), or a date and time such as `2026-09-17T10:30` (New York unless zoned), then pause; responds after analytics and trading settle, with `settled_through`. Meanwhile `stepping` is true and replay writes return `REPLAY_STEPPING`; a target past the recording's end returns 400 and plays nothing |
| `GET /api/replay/history/ID/X`, `DELETE /api/replay/history/ID` | Read a finished run's account, equity history (`account/equity`), portfolio, trades or fills; delete its journal |

Margin settings are returned in account rules. Invalid values or a cash/IRA account
without strategy margin and enforced buying power return HTTP 400 `INVALID_RULES`.
Account-type trading restrictions reject with `ACCOUNT_TYPE`.

This calendar buys the later put and sells the nearer one at a net debit of at most
6.60:

```bash
curl -X POST localhost:8080/api/orders -H 'Content-Type: application/json' -d '{
  "client_order_id": "calendar-1", "type": "limit", "quantity": 1,
  "limit_price": "6.60", "time_in_force": "day",
  "legs": [{"symbol": "SPXW  260925P07700000", "side": "buy"},
           {"symbol": "SPXW  260923P07700000", "side": "sell"}]}'
```

Orders accept `time_in_force: "exto"`, `"gtc_exto"` or `"gtd"` in live and replay
entry/preview routes. EXTO ends with the trading date's last session; GTC_EXTO
works all sessions until expiry/cutoff. GTD trades regular hours until `good_till`,
for example `"2026-09-23T14:15:00Z"`, a future market-time timestamp within 366 days.
Only GTD takes that field. Extended stops are simulator-held triggers that send
limits outside regular hours; stop-limit retains its price, while stop-market
executes once at the touch against displayed size. Extended entries' bracket exits
inherit all sessions. Plain market entries remain IOC and regular-only.

`POST /api/positions/close` and `/api/positions/close/preview` take optional
`type: "limit"` and `limit_ticks: 0` (0–10). Limits reprice from each leg's touch,
work across quotes through the trading date and allow overnight/curb flattening.
A manual limit modification stops repricing. Market remains the default. Orders
report nullable `good_till` and `limit_ticks`; replay mirrors accept the same fields.
Python: `client.place_order(..., time_in_force="gtd", good_till="2026-09-23T14:15:00Z")`
and `client.flatten("SPX", type="limit", limit_ticks=1)`; `preview_flatten` takes the
same pricing arguments.

Sending the same order again with the same `client_order_id` is safe: it returns the
first answer instead of placing a second order, and `GET /api/orders?client_order_id=ID`
finds the order an ID placed. Other terms under a used ID get 409 and record nothing;
an account reset frees the IDs earlier attempts used. A 503 for a full command inbox carries
`Retry-After` seconds; wait that long and send the same body again. [Paper trading](paper-trading.md)
documents every field, rule and reason code.

Sandbox demos also offer `POST /api/sandboxes` without a token. It returns
`{account, token, idle_seconds, simulated: true}` once; it returns 404 when disabled
and 429 at capacity or a creation rate limit. `GET /api/status` includes
`sandboxes: {enabled: true, idle_seconds}` when offered.

Share trades and previews have `/api/replay/stocks/trade` and
`/api/replay/stocks/trade/preview` mirrors, and accept `?account=` live. Trade scope
(or replay scope) is required. Index underlyings are refused. Opening takes the
account's valuation, session, loss, exposure, buying-power and personal checks;
reducing remains available under the kill switch. A reversal is a close plus an
open, accepted or refused as a whole. Share writes have no idempotency key: inspect
positions after an uncertain response before retrying. Preview costs are decimal
money strings; a negative cost receives cash. A refused preview leaves holdings
unchanged. See [shares](paper-trading.md) for margin and defined-risk plan coverage.

## Scripting and agents

The [OpenAPI 3.1 contract](openapi.yaml) describes the terminal API, including
replay mirrors, errors and nullable data. CI validates responses from a running
container with `tools/contract_test.py`, including an order preview and a resting
limit placed and cancelled in an isolated simulated replay.

The [Python package](../python/README.md) has no runtime dependencies. `Client` reads
analytics and accounts, places paper orders and steps replays on market time.
Orders get a client ID; bounded HTTP 503 retries reuse it. Optional extras provide
pandas frames, WebSocket ticks and an MCP server using the official 2.x SDK.

The MCP server gives agents the same paper API. Every tool reports market time,
the provider and its delay, and whether prices are simulated. Writes require
`OPENPORT_WRITE_TOKEN` and an account name. Orders carry an `agent:NAME` tag;
authenticated token names are recorded as actors on journal transactions, orders
and fills. Older journal entries show `unknown`.

Use `--token-file FILE` to give each script its own token. For example, a line
`research read,trade:practice,replay SECRET` permits reads, trading the practice
account and replay controls. Add `--require-token` to protect reads and loopback
writes as well. See [Python setup and examples](../python/README.md).
