# Playbooks

A playbook is a named JSON setup with immutable numbered versions. It selects
listed contracts, checks entry conditions and sizes an order through the normal
server preview. It does not supply market prices. Orders and fills are simulated.

The Playbooks page lists definitions, edits their JSON, selects an account's mode
and compares rule adherence with results. Trade, Brief and Dashboard show staged
orders.
Archived setups retain their stats and saved versions on the page.
The Journal's Playbook filter includes every version of the selected setup; its
Tag filter can narrow that to one version.

## Definition

`POST /api/playbooks` accepts this example. It is a setup, not a recommendation:

```json
{
  "id": "morning-put",
  "name": "Morning put spread",
  "description": "Write the setup and its invalidation here.",
  "underlyings": ["SPX"],
  "window": {"start": "09:45", "end": "11:00", "weekdays": [1, 2, 3, 4, 5]},
  "conditions": {
    "price": {"reference": "day_open", "direction": "above", "value": 0},
    "dte": {"min": 0, "max": 1}
  },
  "structure": {
    "template": {
      "kind": "vertical", "type": "put", "direction": "credit",
      "target": {"mode": "delta", "value": 15}, "width": 5
    },
    "expiry": {"min": 0, "max": 1}
  },
  "sizing": {"units": 1},
  "management": {
    "take_profit_percent": 50, "stop_credit_multiple": 2, "close_by": "15:45"
  },
  "guardrails": {"max_entries_per_day": 1, "cooldown_minutes": 30}
}
```

All top-level fields above are required; `conditions` may be empty. `version` is
assigned on creation. An update must carry the current version; the server checks
it and increments it. Unknown fields, invalid types, nonfinite values, duplicate
symbols or weekdays, and reversed ranges are refused. IDs use 1–15 lowercase
letters, digits or hyphens. Names allow 100 bytes and descriptions 2,000; neither
accepts control characters. There may be 100 IDs, 20 underlyings per definition,
and at most 8 MiB of persisted history. Archived IDs cannot be reused.

Windows use New York market time, including daylight saving time. Weekdays are
1–5, Monday–Friday. Start is inclusive and end exclusive; windows cannot cross
midnight. The normal product session and feed-freshness checks still apply.

| Optional condition | Meaning |
| --- | --- |
| `price` | `reference` is `level`, `prior_close` or `day_open`; `direction` is strictly `above` or `below`. `value` is an absolute level for `level`, otherwise a signed offset in points |
| `iv_rank: {min,max}` | Inclusive rank range, in fractions. Uses the same local and explicitly proxied history as the volatility view; rank can exceed 0–1 |
| `vrp_min` | 30-day model-free IV minus 21-session close-to-close realized volatility, strictly above this many vol points |
| `term_inverted` | Whether model-free 9d/30d is greater than one; false includes equality |
| `dte: {min,max}` | Inclusive ACT/365 calendar days to the selected expiry's settlement |

A missing input fails its condition. Prior close uses the preceding business day's
official close, or its stored daily close. Day open requires the 09:30 minute,
not the first later observation of a partially recorded day. No missing opening
price, IV history, realized volatility or expiry is synthesized. Volatility
conditions use the existing finite-strip and proxy estimates, with the limitations
in [Volatility](volatility.md).

`structure.expiry` selects the earliest unexpired settlement in its inclusive DTE
range. The C++ template picker also serves the terminal; there is one leg-selection
implementation. Its templates are vertical, condor, iron-butterfly, strangle,
straddle, butterfly, calendar and diagonal. Parameters match the Strategy templates
menu: delta is absolute 0–100, widths and offsets are points, and `moves` uses
forward × ATM IV × square root of years. Calendars and diagonals specify the exact
`farExpiry` ID and `offset`. Strikes snap to the nearest listed strike, with ties
to the lower strike. Missing quotes and wings fail without substituting another
contract. Calendar expiry IDs must be updated when moving to another session.

Sizing takes either `units` (1–100,000) or `floor_share` (greater than zero, at most
one). Both are capped by the preview's `max_units`, buying power and account limits.
Fixed sizing uses a floor share of one as its cap. Fractional sizing requires an
actual plan or personal floor. The preview's loss basis is shown: a scenario-grid
loss is an estimate, not a bound for an unbounded structure. A staged net limit
uses executable far sides and rounds to a cent. Submission and fills still run
all normal risk checks.

Management uses the existing combo bracket conventions. `take_profit_percent` is
the closing premium as a percentage of the entry premium: 50 buys back half a
credit; 150 receives one and a half times a debit. `stop_credit_multiple` must be
greater than one and needs a credit entry. Alternatively, `stop_underlying` takes
`level` and `direction: above|below`. Bracket levels are fixed at submission and do
not follow later entry fills. The take-profit limit sits on the legs' smallest tick,
a nickel for SPX, NDX and RUT spreads and a cent for SPY, QQQ and IWM, and rounds to
the better price, so it never asks less than its percent; stop triggers take any cent.

`close_by` is required and must be at or after the window end. Without
`max_hold_days`, it is the entry day's deadline. With that optional 1–365 calendar
day allowance, it is that later day's deadline, moved back to the previous business
day if necessary. It is capped at the day's regular close. A time stop submits a
reducing IOC on the first market update at or after the deadline; it cancels the
entry and its working exits first. Incomplete liquidity or stale quotes can delay
closure, which then fails the time-stop adherence rule.

Guardrails allow 1–1,000 entries per day and a loss cooldown of 0–525,600 market
minutes. Entries include filled and working opening orders across versions of the
same ID, on this account and attempt. A cancelled unfilled entry does not count.
A closed losing strategy starts the cooldown. Personal guardrails remain in force.

## Storage and modes

Definitions and account mode bindings live in `playbooks.json` beside the main
journal. Writes use a temporary file and atomic rename. Every version stays in the
file after editing or archiving. Definitions are not reducer state. Staging and
same-day dismissals are transient; staging does not journal an order, consume
liquidity or alter account state. After restart the server evaluates again.

Each account chooses `off`, `stage` or `auto` for each ID. New definitions start
off. The server evaluates enabled playbooks when the analytics update, about once a
second on a live feed and each market second in a replay, not on every quote. A
stage holds selected contracts, units, exits and a version tag. It expires when
conditions or the window end. Its ID stays the same while the same contracts are
selected; its price, size and exits follow the market. Send re-evaluates, then
submits the stage's latest order through the normal order path, so a click on a
moving market still sends. When other contracts are selected, the old ID is refused
and the new stage is shown. Each entry gets a new client order ID, so a repeat send
cannot duplicate an entry and a later entry is never taken for a retry. Dismiss
suppresses that setup and underlying for the rest of the New York day. Staging
refuses contracts already held or covered by working orders, so separate setups do
not silently share a position.

**Auto mode is refused on live-feed accounts, including practice accounts.** Only
replay and scenario accounts can send entries and time-stop closures automatically.
A rejected automatic entry is suppressed for that day to avoid repeated rejected
orders. Successful entries remain subject to entry limits, overlap and cooldown.
Live time stops are written management rules; the trader must close the position.

ReplayHost copies the live definitions, with modes off, into an isolated run.
Select auto after pausing or before stepping the replay. Changes in that run do not
edit the live catalogue. Its sidecar is `<run>.playbooks.json`; the initial
catalogue and subsequent definition commands also enter run provenance. Offline
verification and archived reports use those recorded versions. Older run inputs
without a catalogue still load. Replay volatility conditions cannot borrow the
live engine's history; unavailable replay inputs prevent entry.

Entries carry `playbook:<id>@v<version>` and the template tag. Their order note
retains the staged preview size, loss estimate, deadline and whether submission
was automatic. A sent stage records the sender as its actor, as any order does;
automatic entries and time stops record `system`. They do not use `Order::system`,
which is reserved for reducer liquidation and would change checks.

## Adherence and results

Reports use the current attempt, grouping a strategy once by the order that opened
its contract round trips. Each trade checks its original version's entry window,
size cap, specified entry exits, time-stop deadline and entry/cooldown guardrails.
A still-open trade's time stop is pending until its deadline. Missing evidence
cannot establish compliance: unknown sizing fails rather than receiving a pass.
Staged entries retain the preview sizing evidence in their immutable order note,
including personal-floor sizing and cross-expiry scenario estimates. Other tagged
entries use their recorded context, measurable strategy risk and entry fees; older
fills without that evidence may not be scoreable. This is a trading review, not an
authentication check on clients supplying their own tags or notes.

Adherence is passed rules divided by measured rules. Closed trades report win
rate (wins divided by all closed trades), average positive and negative net P&L,
expectancy (mean net P&L), profit factor (total wins divided by total losses), and
average R over trades with positive planned risk. Net includes fees. No losses
with wins displays infinity; no observations display missing. Separate summaries
show followed-the-rules and deviating trades. Entry exits are checked as submitted;
the report does not reconstruct every later manual edit to an exit. Manually
mixing contracts from different strategies can make attribution incomplete.

## Pass odds

`GET /api/account/pass-odds?days=20&samples=1000&playbook=morning-put&seed=81723`
returns `pass`, `fail`, `neither`, `median_days_to_pass`, `historical_days`, `seed`,
`days`, `samples`, `block_days`, `history_basis`, `path_assumption` and `label`.
Omit `playbook` for the account. Seed defaults to 81723 and is returned as a decimal
string. Days must be 1–252 and samples 1–10,000. The Dashboard and Playbooks page
label the result **Estimate from past results, not a prediction**.

The bootstrap draws circular blocks of three consecutive completed historical
days, adding each day's equity changes to the attempt's current equity. It uses
recorded open, low, high and close; low and high follow their stored timestamp
order. The recorded extremes begin at a day's first fully marked observation, so
they are widened to include its open and close: a day that gapped and kept going
stays in the sample. At each point it applies the same C++ target, floor-touch, trailing peak,
locking-floor and end-of-day ratchet functions as the trading reducer. End-of-day
plans first close the current day. No future price paths or returns are invented
and labelled as observations. Median days to pass is conditional on passing; it
is null if no sample passes. A decided attempt retains its outcome.

At least ten completed days with recorded extrema are required. Older days
without low/high observations are excluded. Account history is the current
attempt's equity-day history. Per-playbook estimates use only days attributable
exclusively to that playbook: mixed strategies, delivered shares and withdrawals
prevent reliable attribution of the account's intraday low. Those days are
excluded rather than allocating an invented low to one playbook.

The account plan rules are exact; the resampled equity path is an estimate. The
four observations do not describe every intraday excursion or intermediate high,
and block bootstrap does not model a changing edge, changed size or regime. It
has no confidence guarantee and is not a forecast of an evaluation outcome.

## API

All account routes accept `account=ID`, defaulting to `main`. Replay routes use the
usual `/api/replay/` prefix and the replay's account. Existing write protections
apply to every mutation. Creating, editing and archiving definitions and choosing
modes need the `admin` scope; sending and dismissing a stage need the account's
`trade` scope, or `replay` in a replay, as orders do.

| Route | Contract |
| --- | --- |
| `GET /api/playbooks` | Definitions including versions and archive flags, this account's modes, transient stages, reasons and adherence/expectancy reports |
| `GET /api/playbooks/{id}?version=N` | One immutable definition; latest when version is omitted, including archived definitions |
| `POST /api/playbooks` | Create from the definition JSON |
| `PUT /api/playbooks/{id}` | Complete replacement definition with its current `version`; creates the next version |
| `DELETE /api/playbooks/{id}?version=N` | Archive after a version check; keep historical versions |
| `PUT /api/playbooks/{id}/mode` | `{"mode":"off"}`, `stage` or replay-only `auto` |
| `POST /api/playbooks/staged/{stage}/send` | Empty object; recheck and submit the stage's latest order |
| `POST /api/playbooks/staged/{stage}/dismiss` | Empty object; dismiss for this day |
| `GET /api/account/pass-odds` | Seeded, labelled estimate as above; 400 for insufficient history or invalid inputs |
| `GET /api/strategy-template?symbol=SPX&expiry=ID&template=JSON` | URL-encoded template JSON; return legs, tag and actual widths. Optional paired `min_strike`/`max_strike` preserve a terminal's loaded strike window |
