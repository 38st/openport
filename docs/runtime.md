# Runtime and provider operation

`Engine` and `WebServer` permit one `start()` attempt per instance. To restart,
construct a new instance. Engine startup failure stops the provider before its
queue is destroyed. Server shutdown closes the listener and all HTTP/WebSocket
sessions on their executors and drains cancellation handlers before joining.

Polling shutdown checks a shared atomic cancellation flag every 25 ms while
connecting, handshaking TLS, writing, and reading. Pagination, ThetaData endpoint
sequences, and open-interest refreshes check cancellation between requests.
System DNS resolution uses synchronous `getaddrinfo`; shutdown cannot interrupt
it until the operating system returns.

The event queue has a default capacity of 65,536 retained events. Repeated option
quotes, underlying quotes, vendor Greeks, and open interest replace an unconsumed
update of the same kind in place. A contract definition prevents option updates,
including open interest, from coalescing across that definition. Latest-value
state is never dropped: snapshot providers may deduplicate unchanged values and
never resend them. Definitions and provider status are also always retained.

Capacity limits the trade backlog: at capacity, incoming trades are dropped and
incoming state evicts queued trades first. With no trades to evict, state is
retained beyond capacity. Coalescing indexes use drain generations, so ordinary
drains invalidate them without scanning every known instrument or underlying.

Both status and tick JSON expose `engine.queue_depth`, `coalesced_events`,
`dropped_events`, and `overloaded`. Counters are cumulative for the engine's
lifetime; only trades contribute to `dropped_events`. `overloaded` means depth
exceeds capacity or trades have been dropped since the last drain. WebSocket
clients retain the active write and only the newest pending tick, since ticks
contain complete snapshots.

The engine publishes health state and message changes after the batch that
contains them. Otherwise it publishes receipt and error timestamps at most once
every 100 ms, including pending updates when the feed becomes idle. Timestamp-only
updates reuse the published health map and strings.

## Command-line validation

Ports must be integers from 1 through 65,535. Poll intervals and probe timeout
seconds must be positive integers. Expiry counts must be nonnegative integers;
strike windows must be finite numbers in [0, 1]. Numeric suffixes, unknown flags,
and unknown provider option keys are errors. Startup failures print a reason and
exit with code 2.

Supported `openportd --option KEY=VALUE` keys:

| Provider | Keys |
| --- | --- |
| Cboe | `poll_seconds` |
| Massive | `poll_seconds`, `base_url` |
| ThetaData | `poll_seconds`, `base_url` |
| Databento | `quotes=cbbo-1s` or `quotes=cmbp-1`, `trades=on` or `trades=off` |

Databento parent subscriptions stream the entire option chain upstream. Both
CLIs reject nonzero `--expiries` or `--window` with that provider; those filters
cannot reduce upstream traffic.

## Probe readiness

Polling providers need one completed snapshot for **each** requested underlying.
Repeated success for one symbol does not stand in for another. Streaming feeds
need definitions and at least `--quotes N` received option quotes per underlying
(default 100). `--seconds S` sets the overall timeout (default 60); an incomplete
underlying produces a reason and exit code 1. Connection status alone does not
establish streaming readiness. Quote counts reflect retained events after queue
coalescing. Printed chains group by expiry instant and AM/PM settlement, so SPX
and SPXW on the same calendar date occupy separate groups.

## Product sessions and Cboe clocks

`md::trading_session(root, t)` returns regular, curb, global or closed, and the
market time: `t` while open, otherwise the most recent session end at or before
`t`. Regular trading is 09:30–16:15 ET for index roots and SPY/QQQ/IWM/DIA, and
09:30–16:00 for other equities. Early closes are 13:15 and 13:00 respectively.
SPX/SPXW, XSP, VIX/VIXW and RUT/RUTW also have curb trading 16:15–17:00 and GTH
20:15–09:25 Sunday evening through Friday morning. The current Cboe pages list
RUT in GTH as well. See [Cboe hours](https://www.cboe.com/about/hours/us-options/)
and [the GTH description](https://www.cboe.com/insights/posts/cboe-global-trading-hours).

A GTH session is assigned to the following morning's trading date. The calendar
uses the existing 2025–2028 holiday and early-close tables and New York DST.
**Simplifications:** no GTH leading into a holiday (Cboe publishes special holiday
GTH on some dates), no curb on early-close days, and no unscheduled halts.
Outside 2025–2028 only weekdays are known. Trading sessions do not change option
settlement/expiry times. Sunday GTH resumes only when Monday is a trading day;
a holiday evening may open GTH for the following business day.

Cboe option timestamps use `t = publication - 15 minutes`, capped to the most
recent session end only when that option root is closed. Live overnight and
curb quotes therefore advance while the stock/index print remains frozen.
`UnderlyingQuote.ts` is the parsed New York `data.last_trade_time`; missing or
invalid times remain unknown (zero), never fabricated from publication time.
Analytics ignores an underlying print more than 30 minutes behind option data
(`AnalyticsOptions::max_spot_age_minutes`) and infers spot from parity, so frozen
index closes do not contaminate GTH Greeks. The limit sits above the 15-minute gap
between a stock or ETF's 16:00 closing print and its options' 16:15 close.

Status and WebSocket ticks include `{name, open, note}` under each underlying's
`session`. The top-level `market` remains the legacy regular-session calendar.
The as-of badge prefers the selected underlying's session: neutral overnight or
curb labels while open, market closed when closed, and stale only in an open
session when data age exceeds provider delay plus ten minutes.

## Reverse proxies

When a proxy rewrites `Host`, pass each public browser origin explicitly:

```sh
openportd --allowed-origin https://terminal.example.com \
          --allowed-origin http://localhost:3000
```

Origins match exact scheme and authority, with case-insensitive hostnames and
normalised default ports (`https` 443, `http` 80). Paths, credentials, wildcards,
and opaque origins are rejected. Same-origin requests remain accepted, and
non-browser clients may omit Origin. This setting permits WebSocket upgrades; it
does not add CORS response headers.
