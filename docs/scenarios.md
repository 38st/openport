# Scenarios

Scenarios are generated practice sessions, not market data or reconstructions of
historical days. Every generated price is labelled simulated. The session date
sets the calendar and expiries; it does not identify an event being reproduced.

The fourteen built-ins are compiled into the binary from `scenarios/*.json`.
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

All fields except `goal` and `events` are required. Unknown fields and generator
versions are rejected. Files are limited to 64 KiB.

| Field | Meaning and bounds |
| --- | --- |
| `id` | 1–40 lowercase letters, digits or hyphens; no leading or trailing hyphen |
| `title`, `description`, `goal` | Nonempty single lines; title at most 100 bytes, description and goal at most 500 |
| `symbols` | Nonempty unique subset of SPX, SPY and QQQ; overnight supports SPX only |
| `session` | `regular` or `overnight` |
| `date` | A trading date in `YYYY-MM-DD` format |
| `seed` | Unsigned 64-bit integer; zero is a valid seed |
| `generator` | `1`; other versions are refused |
| `drift` | 1–100 `[fraction, cumulative log return]` pairs, with fractions strictly increasing above zero and ending at 1; returns between −0.2 and 0.2 |
| `volatility` | Annualised realised volatility over trading time, 0–1 |
| `iv_shift` | Absolute shift to implied volatility, −0.1 to 1; 0.01 is one vol point |
| `spot_vol` | IV response per unit of spot return, −10 to 0 |
| `events` | At most 32 events; no repeated type at the same time |

Drift is linearly interpolated from zero at the open. A seeded, mean-reverting
wander keeps each run near its path. Regular sessions run from 09:30 to 16:15 ET
(13:15 on early closes); drift reaches its final waypoint at the 16:00 index close
(13:00 on early closes). Overnight sessions run from 20:15 on the calendar evening
before `date` to 09:25 on `date`. Their cash index stays at the preceding business
day's close; options follow the simulated latent level and analytics infers spot
from parity. Prices begin around SPX 6000, SPY 6000/10.02 and QQQ 480.

The five original scenarios keep their original prices, sizes and event order for
the same date and seed. Their old segment moves have been converted to cumulative
waypoints. Tests pin fingerprints of the old recordings.

## Events

Times are `HH:MM` in New York, within the session and before its close. For
an overnight session, 20:15–23:59 belongs to the evening before the trading date;
00:00–09:24 belongs to that date. Events are applied in file order.

| Event | Fields | Effect |
| --- | --- | --- |
| Gap | `{"type":"gap","move":0.01}` | Adds a log return at the opening snapshot; ±0.1 maximum |
| Crush | `{"type":"crush","at":"10:30","iv":-0.04}` | Lowers IV at that time; change from −0.3 to −0.0001 |
| Spike | `{"type":"spike","at":"14:30","move":-0.006,"iv":0.03}` | Spreads a move over five minutes and jumps IV immediately; move ±0.1, IV 0–0.3 |
| Pin | `{"type":"pin","at":"14:30","strike":6000}` | Progressively pulls the SPX level to the strike at the regular close; strike 5400–6600; regular sessions only |

SPY and QQQ follow the changed index path. Every expiry is repriced from the new
forward and smile, including the spot response in `spot_vol`. An IV event receives
weight `0.25 + 0.75 * exp(-days_to_expiry / 7)`: short maturities respond most, while
the far end still responds. Scenarios clamp ATM IV to 2%–200%. Quotes keep
the product's ticks. Gap and spike returns are log returns, so `0.01` moves the
level by about 1.005%.

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
and generator on the same build/platform. This is not a guarantee of identical
fills across playback speeds or consumer scheduling.

Generated recordings are cached by scenario, date and seed in a private temporary
directory, with four completed entries retained. Old entries are removed first;
readers already playing an evicted file keep their open descriptor. The directory
is removed on clean shutdown. The recording header has no extensible metadata, so
scenario id, generator version and seed are kept in replay state and the run's
metadata file beside its journal.

`start_at` also works with `file: NAME` for a recorded feed. It uses New York wall
time on the recording's session date, determined from its first receipt. Evening
receipts belong to the following trading date. Times outside the recorded range
are rejected. A delayed recording still uses receipt time for playback and its
original market timestamps for analytics and the account.

The provider fast-forwards complete receipt groups through the engine, refreshes
analytics periodically and waits for the target group to reach the book, candles
and account before enabling trading. Replay ticks expose `fast_forwarding` and
`progress` (0–1). Writes during this phase return `REPLAY_FAST_FORWARD`.
`paused: true` pauses after preparing that state; without a time it prepares the
opening receipt group. Trading while paused is allowed. EOF makes the run read-only.

## Saved runs

When paper trading and writes are enabled, every run gets a `FileJournal` in
`replays/` beside `--paper-journal`, including runs without an explicit plan (which
use `practice`). Names include the scenario or recording, date, seed or `recording`,
start time and a unique suffix. A JSON sidecar holds the replay metadata. Account
records retain the existing hash chain and schema; repair and compaction commands
include these journals. Runs from a prior process are treated as finished, including
ones interrupted by a crash. Recovery errors are shown in history.

`GET /api/replay` includes a `history` array with pass, fail or open, the plan and
P&L (last marked equity less initial cash). Missing marks are flagged. An open
result means the evaluation is undecided, or the run was practice. It does not
mean that playback is still running. Stopping a run does not liquidate positions;
its final marked positions remain in the journal.

The terminal opens finished runs read-only in Journal and Dashboard, with replay
and simulated/recording labels. The account switcher also lists them. Their routes
are `/api/replay/history/ID/account`, `/portfolio`, `/trades` and `/fills`; they do
not become live accounts. `DELETE /api/replay/history/ID` removes the journal and
metadata; the terminal asks for confirmation. A running run cannot be deleted.
The archive contains account history, not a persisted copy of the replay's chart
or analytics book.

`--no-paper` permits playback without a journal or paper account. A server whose
write policy is disabled permits listing and reading history but refuses replay
starts, controls and deletions. Loopback without a token uses the existing open
write policy; a non-loopback server without a token is read-only.
