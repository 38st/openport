# Scenarios

Scenarios are generated practice sessions, not market data or reconstructions of
historical days. Generated prices and volume are labelled simulated. The session date
sets the calendar and expiries; it does not identify an event being reproduced.

The nineteen built-ins, four of them [several sessions](#several-sessions) long, are compiled into the binary from `scenarios/*.json`.
They need no files at runtime. CMake regenerates the embedded library when a source
file is edited, added or removed; rebuild to change a built-in.
`--scenario-dir DIR` adds JSON files, in
filename order, after the built-ins. A user file with the same `id` replaces that
built-in and is listed with the user files. Invalid user files are skipped with a
file and field diagnostic on stderr. Invalid built-ins fail loading and tests.
Restart the daemon after editing user files. Embedded and user scenarios use the
same strict parser; diagnostics name the source file and field.

## File format

```json
{
  "id": "example-fade",
  "title": "Opening fade",
  "description": "A simulated gap up fades through the session.",
  "goal": "Manage risk through the fading gap.",
  "symbols": ["SPX", "SPY", "QQQ"],
  "session": "regular",
  "date": "2026-09-16",
  "seed": 81723,
  "generator": 1,
  "drift": [[0.25, -0.002], [0.75, -0.006], [1.0, -0.009]],
  "volatility": 0.12,
  "iv_shift": 0.01,
  "spot_vol": -2.0,
  "events": [{"type": "gap", "move": 0.01}]
}
```

All fields except `goal` and `events` are required, unless `sessions` describes the
run (see [several sessions](#several-sessions)). Unknown fields and generator
versions are rejected. Files are limited to 64 KiB.

| Field | Meaning and bounds |
| --- | --- |
| `id` | 1–40 lowercase letters, digits or hyphens; no leading or trailing hyphen |
| `title`, `description`, `goal` | Nonempty single lines; title at most 100 bytes, description and goal at most 500 |
| `symbols` | Nonempty unique subset of SPX, SPY, QQQ, XSP, NDX, RUT and VIX; a single overnight session supports SPX, XSP, RUT and VIX |
| `session` | `regular` or `overnight` |
| `date` | A trading date in `YYYY-MM-DD` format |
| `seed` | Unsigned 64-bit integer; zero is a valid seed |
| `generator` | `1`; other versions are refused |
| `drift` | 1–100 `[fraction, cumulative log return]` pairs, with fractions strictly increasing above zero and ending at 1; returns between −0.2 and 0.2 |
| `volatility` | Annualised realised volatility over trading time, 0–1 |
| `iv_shift` | Absolute shift to implied volatility, −0.1 to 1; 0.01 is one vol point |
| `spot_vol` | IV response per unit of spot return, −10 to 0 |
| `events` | At most 32 events; no repeated type at the same time |
| `sessions` | 1–24 sessions played one after another as one run, in place of `session`, `drift` and `events`; see below |

Drift is linearly interpolated from zero at the open. A seeded, mean-reverting
wander keeps each run near its path. Regular sessions run from 09:30 to 16:15 ET
(13:15 on early closes); drift reaches its final waypoint at the 16:00 index close
(13:00 on early closes). Overnight sessions run from 20:15 on the calendar evening
before `date` to 09:25 on `date`. Their cash index stays at the preceding business
day's close; options follow the simulated latent level and analytics infers spot
from parity. Prices begin around SPX 6000, SPY 6000/10.02 and QQQ 480.
Revision 3 adds XSP at SPX/10, NDX near 21000 following QQQ's moves, RUT near
2300 with a 1.15 beta and its own seeded wander, and VIX at the generated SPX
30-day ATM implied volatility in points. Spikes and crushes move VIX with that
volatility. VIX options use a forward mean-reverting toward 19.5, with a rising
call wing; they do not apply a stock dividend yield to VIX spot.

The `symbols` field selects which chains a scenario generates. The `index-spike`
built-in lists all seven underlyings on 17 September 2026, the last trading day
of that month's SPX, NDX and RUT AM series. Existing built-in JSON sources and
hashes are unchanged.

| Underlying | Series and expiries | Quote ticks |
| --- | --- | --- |
| SPX | SPXW PM dailies/weeklies; SPX AM third-Friday monthlies | $0.05 below $3, $0.10 from $3 |
| XSP | XSP PM dailies, weeklies and monthlies | $0.01 below $3, $0.05 from $3 |
| NDX | NDXP PM dailies/weeklies; NDX AM third-Friday monthlies | $0.05 below $3, $0.10 from $3 |
| RUT | RUTW PM dailies/weeklies; RUT AM third-Friday monthlies | $0.05 below $3, $0.10 from $3 |
| VIX | VIX AM monthlies; VIXW AM Wednesday weeklies | $0.01 |
| SPY, QQQ | PM ETF series | $0.01 |

VIX monthlies settle 30 days before the next month's third Friday (using the
preceding business day if that Friday is a holiday); a settlement holiday moves
the expiry to the preceding business day too. VIXW weeklies use Wednesdays,
also moved back on holidays, and omit monthly dates. Two upcoming monthlies and
two weeklies keep the chain bounded, alongside series carried from earlier dates.

Each chain lists the date's own expiries (the date, the next two business days, the
Friday after and next month's third Friday, AM-settled for SPX) and every series an
earlier date listed until its last trade, as wide as it was then: a position opened
on one day, as the demo feed carries them from day to day, keeps its quotes until it
expires. The recording opens with each underlying's previous close, dated the
preceding business day at those opening levels (before any gap), so market-wide
circuit breakers measure a fall from it. Generator revision 2 added both; the
contracts revision 1 listed keep their identifiers, quotes and sizes, and a run
recorded before revisions regenerates revision 1. Revision 3 adds the index families,
product tick rules, sessions and VIX term structure. Runs recorded at revision 1, 2
or 3 regenerate their original events, including quote sizes and volume; new
underlyings require revision 3. The JSON `generator` stays at version 1.

Revision 4 prices SPY and QQQ options as American: Black on a cash-dividend-adjusted
forward plus a 13-step Leisen–Reimer early-exercise premium for ITM puts and calls
with a payment before expiry, floored at intrinsic value. Index options remain
European. The small odd tree cancels its European discretisation error against the
same lattice; it is an approximation, refreshed at every snapshot. ETF forwards use
spot less the present value of known cash dividends, grown at 4%, with no flat dividend
yield. Quotes still use the product's ticks. Revisions 1–3 keep their original prices.

The simulated quarterly calendar has SPY go ex on the third Friday of March, June,
September and December, and QQQ on the following Monday. A holiday moves either to
the next business day using the exchange calendar. Payments are about 0.3% of SPY's
6000/10.02 opening reference and 0.15% of QQQ's 480 reference (about $1.80 and $0.72),
with a date-and-symbol-keyed variation of ±5%, rounded to whole cents. Neither the
scenario nor its seed changes that date's amount. These are simulated payments,
not actual declarations. Known payments through the listed expiries affect options,
including payments after the last session. On an ex-date the ETF price drops by the
payment; the preceding close remains cum-dividend, including when the run starts
on the ex-date.

An explicit session `dividends` entry replaces the generated payment for that symbol
and date, in prices and the Replay account. The resulting scenario calendar replaces
`--dividends` payments on matching symbols and dates; other server payments remain.
The complete merged schedule is recorded in the run's start record for verification
and resume. In the real-time demo feed the generated calendar supplies account
payments and warnings when no `--dividends` source was given; an explicit source
supplies the account calendar instead (it does not change generated prices).

AM series trade through the business day before settlement, stopping at its
regular option close (16:15 ET, 13:15 on early closes). Revision 3 retires their
quotes there, so the following curb and overnight sessions cannot quote them.
They are absent from the settlement date's chain: SPX, NDX and RUT monthlies
settle at the open while same-date SPXW, NDXP and RUTW PM series still trade to
16:00. XSP is always PM, including its third-Friday expiry. VIX and VIXW are
both AM: their usual last trading day is Tuesday before Wednesday settlement,
with the business-day adjustment applied before choosing the last trade.

In driver-5 replays, an AM position held from the preceding day settles on the
underlying's first print at or after 09:30 ET on the expiry date, even though the
option is absent from that day's chain. `scenario_opening_print` records its
provider, symbol and quote time. This approximates the official special opening
quotation; it is not an official settlement. Recorded replays use the same rule
with `recorded_opening_print`. Drivers 1–4 retain manual AM settlement when verified.
The generator revision and its market events are unchanged.

At revision 1 the five original scenarios keep their original prices, sizes and relative order
of existing events for the same date and seed. Their old segment moves have been
converted to cumulative waypoints. Tests pin fingerprints of the old events.

Session volume starts at zero and accumulates deterministic simulated activity,
higher near the money and in the front expiry, with more activity near the open
and close. Its separate seeded draws leave prices and quoted sizes unchanged.
Volume events are recorded alongside quotes for built-in and custom scenarios.

## Several sessions

A scenario can span several sessions and days, played as one run on one account:
hold across the close, through the curb and overnight sessions, over a weekend or
for a week. The end-of-day ratchet, rollover, GTC carry, early assignment of deep
shorts, ex-dividend dates and the gap at the next open then happen inside a replay
you can pause and step, rather than only on the real-time demo feed.

```json
{
  "id": "hold-overnight", "title": "Hold through the night",
  "description": "SPX firms into the close and opens the next day lower.",
  "symbols": ["SPX", "SPY"], "date": "2026-09-16", "seed": 81738, "generator": 1,
  "volatility": 0.11, "iv_shift": 0, "spot_vol": -2,
  "sessions": [
    {"session": "regular", "drift": [[1, 0.005]]},
    {"session": "curb", "drift": [[1, -0.001]], "volatility": 0.06},
    {"session": "overnight", "drift": [[1, -0.005]],
     "events": [{"type": "spike", "at": "02:30", "move": -0.003, "iv": 0.015}]},
    {"session": "regular", "drift": [[1, 0.005]], "events": [{"type": "gap", "move": -0.003}],
     "dividends": [{"symbol": "SPY", "per_share": 1.75}]}
  ]
}
```

With `sessions`, the file omits `session`, `drift` and `events`; its `volatility`,
`iv_shift` and `spot_vol` apply to each session that does not give its own. Each
session has:

| Field | Meaning and bounds |
| --- | --- |
| `session` | `regular`, `curb` or `overnight` |
| `drift` | As above, over this session: zero is where the previous session ended |
| `volatility`, `iv_shift`, `spot_vol` | Optional; as above |
| `events` | Optional; as above, at times inside this session. A pin needs a regular session |
| `dividends` | Optional; at most two `{"symbol": "SPY", "per_share": 1.75}`: SPY or QQQ, among `symbols`, going ex on this session's trading date. Not on the run's first date, and once a symbol and date |

`date` is the first session's trading date, and the sessions follow each other in
time:

- The first session is regular or overnight.
- An overnight session is followed by its own date's regular session.
- A curb session follows its date's regular session, from 16:15:15 to 16:59:45
  every 15 seconds; an early-close date has none.
- Any other session after a regular or curb one belongs to the next trading date:
  an overnight session after Friday's regular one runs from Sunday at 20:15 into
  Monday, skipping weekends and holidays.

A sequence the calendar cannot hold is refused with the session named. In revision
3, curb and overnight sessions trade SPX, XSP, RUT and VIX options, following the
product calendar; such a scenario must list at least one of them. NDX, SPY and QQQ
may still be listed in multi-session scenarios: they and their options quote only
in regular sessions, and their chains report `options closed` in between. Cash
index prints stop at the regular close; options follow latent levels outside it.
Revisions 1 and 2 retain their original SPX-only extended-session output.

Each session starts where the one before ended, plus its own gap, and its seeded
wander starts afresh. Crush and spike IV changes last for the rest of the run. Each
new trading date lists its own expiries, centred on the level it opens at, keeps
every earlier series until its last trade, moves open interest on with the days to
expiry and starts session volume again. It also opens with each underlying's
previous close as the run printed it, which circuit breakers measure against. An
ETF's price drops by its dividend from the ex-date on, and the run's account pays
or charges the dividend on shares held into that date, in place of any dividend the
server knows for the same symbol and date. The recording polls at its slowest
session's interval, a minute when it has an overnight session.

Multi-session scenarios are generated at revision 2 and later. Earlier revisions
produce the same recording events as before, byte for byte. The real-time demo
feed (`--provider demo`) already carries its account from day to day and plays
single sessions only, so it skips these scenarios; so do playbook backtests, whose
days are single sessions.

## Events

Times are `HH:MM` in New York, within the session and before its close (a curb
session's close is its last snapshot, 16:59:45). For
an overnight session, 20:15–23:59 belongs to the evening before the trading date;
00:00–09:24 belongs to that date. Events are applied in file order.

| Event | Fields | Effect |
| --- | --- | --- |
| Gap | `{"type":"gap","move":0.01}` | Adds a log return at the opening snapshot; ±0.1 maximum |
| Crush | `{"type":"crush","at":"10:30","iv":-0.04}` | Lowers IV at that time; change from −0.3 to −0.0001 |
| Spike | `{"type":"spike","at":"14:30","move":-0.006,"iv":0.03}` | Spreads a move over five minutes and jumps IV immediately; move ±0.1, IV 0–0.3 |
| Pin | `{"type":"pin","at":"14:30","strike":6000}` | Progressively pulls the SPX level to the strike at the regular close; strike 5400–6600; regular sessions only |

SPY, QQQ, XSP, NDX and RUT follow the changed index path. Every expiry is repriced from the new
forward and smile, including the spot response in `spot_vol`. An IV event receives
weight `0.25 + 0.75 * exp(-days_to_expiry / 7)`: short maturities respond most, while
the far end still responds. Scenarios clamp ATM IV to 2%–200%. Quotes keep
the product's ticks. Gap and spike returns are log returns, so `0.01` moves the
level by about 1.005%.

## Playbook runs

Replay and scenario accounts may enable [playbooks](playbooks.md) in auto mode.
Pause a new run, enable its copied definition and step the market clock. Entries
use the normal preview, risk and order paths; time stops submit reducing orders at
their market-time deadlines. Definitions and commands enter run provenance, so the
same scenario, seed and commands reproduce the fills and journal. Live-feed accounts
cannot use auto mode. Missing volatility history in a replay prevents an entry
condition from passing; it never borrows future or live-feed history.

## Runs and drills

`POST /api/replay` accepts `scenario: ID` (or the compatible `demo: ID` / `demo:
true`), `plan`, `speed`, `date`, `seed`, `start_at` and `paused`:

```json
{"scenario":"afternoon-waterfall","plan":"intraday-50k","speed":60,
 "seed":"81723","start_at":"14:25","paused":true}
```

Omit `seed` for a fresh seed from the host's `std::random_device`. Use
`"seed":"scenario"` for the file's seed, or an unsigned integer or decimal string
for a repeat. API seed values are returned as strings to preserve all 64 bits in
JavaScript. The generator uses its own deterministic draws; the engine and reducer
do not choose seeds. Repeatability is for the same scenario contents, date, seed
and generator on the same build/platform. Replay batches, fills and journal bytes
are independent of playback speed and consumer scheduling when commands occur at
the same market times. `--verify-run JOURNAL` regenerates and checks a saved run.

Generated recordings are cached by scenario, date and seed in a private temporary
directory, with four completed entries retained. Old entries are removed first;
readers already playing an evicted file keep their open descriptor. The directory
is removed on clean shutdown. The recording header has no extensible metadata, so
scenario id, source hash, generator version and revision, date and seed are recorded
in the account journal. Replay state and the JSON sidecar also retain the display metadata.

`start_at` is New York `HH:MM[:SS]`, or a date and time such as
`2026-09-17T10:30` (seconds optional), New York unless it ends in `Z` or a UTC
offset. In a scenario of several sessions, a bare time is its first occurrence in
any of them; a time between sessions, or a date and time outside the run, is
refused. Replay state, the listing's `demos` and history entries carry the run's
`sessions` (each with `session`, `date`, `open` and `end` timestamps) and its `end`;
a recording has none. Between sessions, paced playback waits at most one polling
interval of receipt time, so a closed night or weekend passes in a moment at any
speed, and `PUT {"until": ...}` accepts the same forms, a bare time being its next
occurrence at or after the replay's time in the run's sessions.

`start_at` also works with `file: NAME` for a recorded feed. It uses New York wall
time on the recording's session date, determined from its first receipt. Evening
receipts belong to the following trading date. Times outside the recorded range
are rejected. A delayed recording still uses receipt time for playback and its
original market timestamps for analytics and the account.

The provider fast-forwards complete receipt groups through the engine, refreshes
analytics at deterministic market boundaries and waits for the target group to reach the book, candles
and account before enabling trading. Replay ticks expose `fast_forwarding` and
`progress` (0–1). Writes during this phase return `REPLAY_FAST_FORWARD`.
`paused: true` pauses after preparing that state; without a time it prepares the
opening receipt group. Trading while paused is allowed. EOF makes the run read-only.

Relative steps (`until:"+15s"`, `"+1m"`, `"+1h"`) and `until:"next"` make repeatable
small advances. Abort, pause and stop interrupt a long step after its current batch
settles. A paused skip is queued visibly as `skip_pending`; `skip:false` cancels it.

`play_until:"14:30:15"` with optional `speed:60` watches an advance and pauses at
the same settled target as `until`; state shows `pause_at` while it plays. Manual
speed, pause, until, abort or stop clears that target.

## Restarting a drill with its commands

`POST /api/replay {"restart":"ID","at":"10:30:15"}` starts a **new** durable run
using the source's recording or scenario, date, seed, revision, plan, copied risk
settings, analytics, dividends and playbook definitions. It re-executes the source's
journaled commands **through T inclusive**, keeping the identical journal prefix;
commands after T are not scheduled. Then it pauses with the book, analytics and
account settled through T. Later orders belong only to the new run. `at` accepts
New York `HH:MM[:SS]` or a dated time (New York unless zoned). Omit it to restart at
the source's own start. Speed defaults to the source's; optional `paused:false`
continues after rebuilding the prefix. An active source is replaced and archived;
its journal is unchanged. The new sidecar/history names `restarted_from:{id,at}`.
The terminal offers **Restart from…** on active and finished runs.

Targets before the source's start or after its last settled market time are 400;
unknown ids are 404. Verification of the source in progress is 409
`VERIFICATION_RUNNING`. Missing/changed recordings or scenarios, older drivers,
changed calendars, torn/inconsistent journals and runs that changed their recording
part way return 409 `REPLAY_NOT_RESTARTABLE`. These refusals preserve the source.
Restart uses driver 6's existing recovery and byte verification; old sidecars need
no new fields. The new journal passes the same `--verify-run` as the source.
`start_at` for ordinary starts still names receipt time; restart `at` names market
time, so delayed recordings do not move commands to their receipt timestamps.

## Saved runs

When paper trading and writes are enabled, every run gets a `FileJournal` in
`replays/` beside `--paper-journal`, including runs without an explicit plan (which
use `practice`). Names include the scenario or recording, date, seed or `recording`,
start time and a unique suffix. A JSON sidecar holds the replay metadata. Account
records retain the existing hash chain and schema; repair and compaction commands
include these journals. Runs from a prior process are treated as finished, including
ones interrupted by a crash, until one is resumed (below). Recovery errors are shown in
history. A run that has
ended trades no more, so its archive shows every order it left working or armed (a
resting GTC limit, an armed stop) cancelled with `RUN_ENDED`. Its journal keeps those
orders as they were.

`GET /api/replay` includes a `history` array with pass, fail or open, the plan's
id (`plan`) and name (`plan_name`) and P&L (last marked equity less initial cash).
Missing marks are flagged. An open result means the evaluation is undecided, or
the run was practice. It does not mean that playback is still running. Each entry
carries the run's final playback state: stopping or replacing a run rewrites its
sidecar, so `fast_forwarding` is false and `settled_through` is where it stopped. A
run a crash interrupted reports `progress` 1, paused, and its last journaled time as
`settled_through`, and `interrupted` true. Stopping a run does not liquidate
positions; its final marked positions remain in the journal.

### Resuming an interrupted run

`POST /api/replay {"resume": "ID"}` continues a run a crash interrupted (`interrupted`
true; the terminal's Resume button). It plays the same recording, or regenerates the
same scenario day, and re-executes the run's recorded inputs in order, unpaced, as
`--verify-run` does: each batch's boundary is checked against the batch the recording
delivers, and each transaction against the journal's through its hash. Nothing is
written until the re-execution reaches the journal's end, so the run continues on the
same journal, with the same account, positions and working orders, and the journal
reads as if the run had never stopped: it still verifies. The run continues from the
last market time it recorded, paused unless `"paused": false`; `speed` may be given
too. Its equity history and playbook catalogue are rebuilt as it re-executes.

A run that played to its end or was stopped has ended and returns 409
`REPLAY_NOT_RESUMABLE`, as does one recorded by an older build's driver, one that
changed its recording part way, or one whose recording, scenario, generator or the
exchange calendar has changed since it started. When re-executing reaches a batch or
transaction its journal does not hold, the run stops trading with the reason, and its
journal is left as it was.

The terminal opens finished runs read-only in Journal and Dashboard, with replay
and simulated/recording labels. The account switcher also lists them. Their routes
are `/api/replay/history/ID/account`, `/account/equity`, `/portfolio`, `/trades` and
`/fills`; they do not become live accounts. The equity history is read from the
`ID.jsonl.equity.csv` beside the journal and never rewritten.
The terminal offers **Verify** on finished runs. POST
`/api/replay/history/ID/verify` starts a background verification; GET reads its
`idle/running/passed/failed` state and GET `?format=receipt` downloads the result.
History includes the latest `verification`, persisted across restarts and invalidated
if the journal's size or modification time changes. A second verification returns
409 `VERIFICATION_RUNNING`; an active replay cannot be verified. Verification uses
replay write scope, reads use read scope, and disabled writes refuse starts.
See [run verification](runtime.md#verifying-a-run) for receipt fields and limits.

History listings and account reads compare the saved final journal head/count/bytes
with the current journal. `torn` and `bytes_cut` identify an incomplete final line;
`truncated`/`mismatch` and `integrity_message` identify missing or changed records.
The warning names the run, expected/found counts and `--repair-journals` for torn
lines. Older runs without a final checkpoint still load.

`DELETE /api/replay/history/ID` removes the journal, its metadata, playbooks and
equity-history sidecars, whether or not the journal still verifies, so an edited or
torn run can be deleted too; the terminal asks for confirmation. A running run cannot
be deleted (409 `REPLAY_RUNNING`), including one another openportd is writing. An id
that names no saved run returns 404 `NOT_FOUND`; a journal that cannot be opened
returns 422 `REPLAY_HISTORY_FAILED` with the reason.
The archive contains account history, not a persisted copy of the replay's chart
or analytics book.

`--no-paper` permits playback without a journal or paper account. A server whose
write policy is disabled permits listing and reading history but refuses replay
starts, controls and deletions. Loopback without a token uses the existing open
write policy; a non-loopback server without a token is read-only.
