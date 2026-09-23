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
| Replay | `file=PATH` (required), `speed=1`, `10`, `60` or `max`, `loop=on` or `off` |

Databento parent subscriptions stream the entire option chain upstream. Both
CLIs reject nonzero `--expiries` or `--window` with that provider; those filters
cannot reduce upstream traffic.

## Recording and replay

Record any provider before the engine's coalescing queue:

```sh
./build/apps/openportd --provider cboe --symbols SPX,SPY --record session.oprec
./build/apps/openportd --provider replay --symbols SPX \
  --option file=session.oprec --option speed=10 --option loop=off
./build/apps/openport-probe replay SPX --option file=session.oprec --option speed=max --analyze
```

Both CLIs accept `--record FILE`; the probe also accepts repeated `--option KEY=VALUE`.
`openportd --record-dir DIR` records each run to its own file there, named by provider
and UTC start time (`cboe-2026-09-23T184820Z.oprec`), creating the directory if needed.

Full chains are large. Cboe's first poll of the whole SPX and SPY chains, about 164,000
events (definitions, quotes, open interest and Greeks), recorded as 2.7 MB, about 16
bytes an event. While the market trades, most quotes change on every 15-second poll,
so a full day of SPX, SPY and QQQ can run to gigabytes; `--expiries` and `--window`
narrow what is recorded.

Files are created exclusively with mode 0600: an existing pathname, including a
symlink, fails at startup and is never overwritten. For `--record FILE`, parent
directories must exist. The original provider name, capabilities, subscription and
engine start wall time are saved; API keys and provider configuration options are
not stored. Provider status text is recorded verbatim, so recordings inherit
anything a provider puts in those messages.

Replay defaults to `speed=1` and `loop=off`. The first event starts immediately;
subsequent receipt-time gaps use absolute monotonic deadlines, divided by the speed
(1, 10 or 60 from the command line; 1, 2, 5, 10, 30, 60, 120 or 300 in the terminal),
carrying fractional nanoseconds so rounding does not accumulate. `max` skips waits.
A running replay can change speed, rescaling the wait in progress; pause, where the
paused time does not count against the gap; and skip, which plays the next event at
once. A backward receipt-clock step contributes zero delay; positive subsequent
gaps still count. Waiting is interruptible at every speed. Filtering keeps gaps
across omitted symbols and retains provider-wide status. Unknown symbols fail
synchronously and list the subscription in the header; the header declares what
was subscribed, not whether every symbol ultimately produced quotes. New expiry
or strike-window filters are rejected: the file already reflects the recording's
original filters.

The provider is named `replay (<original provider>)` and exposes the original
capabilities. Every selected event keeps its original fields and market timestamp;
analytics uses the recorded market time. A clean end adds `Stopped`, allowing the
engine to finish its analytics immediately and keep the result visible. Looping
rewinds the same open file, resends definitions and repeats the recorded events
and their gaps, with no extra gap between passes. Empty recordings stop even with
loop enabled. A truncated recording stops with its recovery diagnostic and never
loops; malformed data produces `Error`. Neither the daemon nor its web terminal
exits automatically at replay EOF. The probe retains its readiness/timeout rules
and stops waiting at EOF if it has not become ready.

**Format v1.** All integers are fixed-width little-endian; signed integers use
two's complement. Strings are a `u32` byte length followed by those bytes (no
terminator or encoding conversion). Doubles are IEEE-754 binary64 bits, preserving
NaN payloads, infinities, subnormals and signed zero. There is no native struct
padding, locale-dependent text, or lossy numeric conversion.

The uncompressed prefix is eight magic bytes `OPREC\r\n\0`, a `u32` version
(currently 1), and a `u32` header-payload length. The payload, in order, is:

| Field | Encoding |
| --- | --- |
| Provider | String |
| `realtime`, `realtime_plan_dependent` | Two `u8` booleans, 0 or 1 |
| Poll interval, delay | Two `i64` counts of seconds |
| Quotes, trades, open interest, vendor Greeks, history | Five `u8` booleans |
| Subscription | `u32` symbol count, then strings, `i32` max expiries, binary64 strike window |
| Start wall time | `i64` Unix nanoseconds |

The rest is concatenated independent zstd frames (level 1, content checksum).
Their decompressed contents are a stream of `u32` record-payload lengths followed
by payloads. A payload begins with `i64` local receipt Unix nanoseconds, then a
`u8` tag and the fields below. `id` is `u32`, `ts` is `i64` Unix nanoseconds,
prices/sizes/Greeks are binary64, and strings use the encoding above.

| Tag | Event | Fields, in order |
| --- | --- | --- |
| 0 | Contract definition | id; root; underlying; expiry year, month, day (three `i32`); strike; type (`u8`: call=0, put=1); style (`u8`: European=0, American=1); settlement (`u8`: AM=0, PM=1); multiplier; standard (`u8` boolean) |
| 1 | Option quote | id, ts, bid, ask, bid size, ask size |
| 2 | Option trade | id, ts, price, size |
| 3 | Open interest | id, ts, contracts |
| 4 | Vendor Greeks | id, ts, iv, delta, gamma, vega, theta, rho |
| 5 | Underlying quote | symbol, ts, bid, ask, last |
| 6 | Provider status | ts; state (`u8`: Connecting=0, Live=1, Delayed=2, Stale=3, Error=4, Stopped=5); message; underlying |

A zero record length is the clean-end marker, written only after all publishers
stop and all accepted events drain. It must finish the final frame; bytes after
that frame are rejected. Headers and record payloads are limited to 1 MiB;
decoder zstd windows are limited to 16 MiB. Unknown versions/tags, invalid enums,
invalid lengths, extra fields and corrupt/checksum-failed frames are explicit
errors. The reader streams with bounded buffers rather than loading the file.
Physical EOF without a clean marker, including a torn final compressed frame,
returns all recoverable complete records and a `recording truncated` diagnostic.
A partial final record is discarded; a recovered incomplete frame cannot have
its checksum verified. Even a crash exactly between frames is reported as
truncation rather than mistaken for successful shutdown.

**Ordering, durability and limits.** One publisher lock establishes the same
order for receipt timestamps, recording and downstream delivery, including
concurrent publishers. Recording never coalesces events or drops trades.
Serialization appends to a reusable buffer. A worker seals/writes a frame every
second (including idle feeds) or at 4 MiB of raw records, whichever comes first;
a frame can exceed that threshold by one record. Publications backpressure while
a sealed frame is being compressed/written, keeping at most one outstanding
frame. A process crash can therefore lose at most that outstanding frame, with
partial recovery possible. Slow or blocked storage stalls publication instead of
silently losing data. These are process-crash guarantees for completed OS writes,
not power-loss durability: there is no per-frame `fsync`, and OS scheduling or
blocked storage can delay a flush. Recording memory stays bounded independent of
session duration; the downstream book and replay's contract-to-symbol map still
grow with the number of defined contracts.

A serialization, write or close failure stops recording, reports its explicit
reason through provider status, and remains an `Error` in engine status even after
healthy feed updates. Live analytics continues; the daemon logs the reason and
returns 1 on shutdown. The saved file is a recoverable prefix, not a complete
session. Startup failures return 2. Clean shutdown stops the provider first,
drains/closes the recording, and drains the engine before its final analytics.
The daemon prints accepted events, bytes, average sink nanoseconds/event and
bytes/event on shutdown. Counters include accepted but not durable events if
storage failed.

The recording and replay provider are lossless; the engine/probe's existing
latest-value queue still coalesces updates and can drop trades under overload.
Consequently final analytics for the same data and analytics settings are
reproducible, while intermediate UI snapshots, book version counters, health
receipt times and compute durations depend on consumer scheduling. Looping
repeats into the existing book; it does not reset analytics or rewind the book's
maximum observed market time. Use a fresh Engine and one pass for regression
comparisons. Paper trading on a replay sees the same coalesced batches, so its fills
are not a deterministic per-event simulation. Replay stop interrupts pacing immediately;
as with other providers, it cannot interrupt a blocked OS file read or a sink
that itself blocks.

**Measured synthetic cost.** On this Apple M2 Max, Release/Apple Clang, the
`RecordingPerformance.SyntheticChain` test publishes a 30,000-contract SPXW chain
(60 expiries × 250 strikes × two sides), then 30 quotes per contract: 930,001
events including spot. A measured run wrote 3,108,528 bytes (2.96 MiB), or **3.342
bytes/event**, including header, checksums and end marker. Publisher sink time
was **231 ns/event**, including queue delivery and backpressure; the complete
publish-and-close run was 262 ns/event versus 18 ns/event for the same events
through a queue alone, approximately **244 ns/event added**. This regular,
repetitive synthetic chain compresses well; real market prices, timestamps,
trade mixes and status strings can require substantially more space. Receipt
timestamps come from the real local clock, so compressed sizes vary between
runs. These averages include compression/write stalls but are not tail latency
or stable-storage benchmarks.

Reproduce without network access:

```sh
./build/tests/openport_tests --gtest_filter=RecordingPerformance.SyntheticChain
```

Recording uses the existing system zstd dependency, including when the Databento
adapter is disabled; no additional Docker packages are required.

### Replaying in the terminal

A running `openportd` replays recordings beside its live feed, one at a time. The
Replay page (or `/api/replay`) lists the files in `--record-dir` (default
`~/.openport/recordings`), starts one at 1, 2, 5, 10, 30, 60, 120 or 300 times real
time or as fast as possible, and pauses, resumes, changes speed or skips a gap such
as an overnight close while it plays. Each replay has its own engine, an in-memory
practice account and chart history, all on the replay's clock: the recorded receipt
times, so sessions, the feed delay and paper-trading gates behave as they did that
day (a recording of a stalled feed replays as stalled). "Trade this replay" points
every page at it, `/api/X` becoming `/api/replay/X`, until "Back to live"; the live
accounts keep trading meanwhile. Stopping the replay discards its account. Recording
names are plain file names inside the directory; anything else is refused.
Without a replay, `/api/replay/...` returns 404 `NO_REPLAY`; a recording that cannot
be read fails to start with 422 `REPLAY_FAILED`.

## Price history

The chart on Trade reads `GET /api/underlyings/{symbol}/candles` from two sources:

- **The engine's own spot.** Every analysis adds the spot it priced with to a
  one-minute bar, stamped with the time of that price: the underlying's print time
  when spot is a quote, the option data's market time when it is inferred from
  parity. Delayed feeds therefore land on the minute the price was traded, not the
  minute it arrived, and an index frozen at its close adds nothing overnight while
  its parity spot moves with the global session.
- **Cboe's free chart files.** `openportd` fetches the latest regular session's
  one-minute bars (every minute while a session is open or just closed, every 15
  minutes otherwise) and the daily history (hourly) for each symbol. An official
  minute replaces a sampled one and later samples never change it. Cboe labels a
  minute bar by the minute it closes; OpenPort keys bars by the minute they open.
  Symbols Cboe does not chart (HTTP 403 or 404) are retried hourly. `--no-history`
  turns the fetch off.

Coarser intervals group minutes: 5, 15 and 30 minutes from the top of the hour,
and hours from half past, so the first hourly bar starts at the 09:30 ET open.
Daily bars are Cboe's; a session it has not published yet is built from its
regular-hours minutes (09:30 to the close, 13:00 on early-close days).

Finished minutes persist in `--candle-dir` (default `~/.openport/candles`), one
`SYMBOL.csv` per underlying with `start seconds,open,high,low,close,source` lines,
where the source is `o` for official and `s` for sampled and a later line for the
same minute wins. The directory keeps the last ten days of minutes; files are
compacted on startup and whenever superseded lines outnumber current ones. Daily
bars are refetched rather than stored. A replay keeps its bars in memory and never
fetches history, so a recorded day's prices do not mix with live ones. Storage
failures are printed and never stop the engine.

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
