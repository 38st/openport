# Paper trading core (v1)

`openport::trading` is a C++20 library linked as `openport::trading` / `openport_trading`.
It depends on `openport_core`; JSON and OpenSSL Crypto are private dependencies.
There are no threads, network calls or wall-clock reads. Only `FileJournal` accesses
files. Caller timestamps are **effective market time**, UTC nanoseconds, nonnegative
and monotone. Delayed feeds must pass their delayed market time, not receipt time.

## Public API and integration

| Header under `include/openport/trading/` | API |
| --- | --- |
| `money.hpp` | `Money::parse`, `from_micros`, `from_double`, `str`, checked arithmetic and `prorate` |
| `types.hpp` | `Reason`, `TradingError`, `Decision`, contracts/quotes/valuation checks, orders, fills, limits, configuration |
| `ledger.hpp` | `Account`, `Position`, `Ledger::fill`, `settle`, `restore` |
| `risk.hpp` | `portfolio_risk`, `check_exposure`, `scenario_grid`, risk buckets and scenario cells |
| `journal.hpp` | `Journal`, `FileJournal::create/read/resume`, `verify_journal`, `JournalRecovery` |
| `session.hpp` | `TradingSession`, `CommandResult`, immutable `TradingSnapshot` |

Typical ingress:

```cpp
using namespace openport::trading;
auto journal = FileJournal::create("paper.jsonl");
TradingSession session(SessionConfig{}, market_time, journal);
session.define(resolved_contract, market_time);
session.on_quotes(quote_batch, valuation_batch, market_time);
auto result = session.submit(order_request, market_time);
auto published = session.snapshot(); // shared_ptr<const TradingSnapshot>
```

`define` accepts an already resolved `md::OptionContract`. Parsing an OSI string
alone is not proof of a known listed definition; that responsibility belongs to
the caller. All subsequent references use its **canonical padded OSI**, never a
provider's dense instrument ID. Re-registering identical terms is harmless;
conflicting terms under one OSI reject.

Reducer commands are `define`, `submit`, `cancel`, `on_quotes`, `set_limits`,
`trip_kill`, `reset_kill`, `settle`, and `roll_day`. Every completed command, including
a business rejection, increments `account_version`. Business failures return a
`Decision` with code/message and numeric actual/limit/scope where applicable.
Invalid command batches, arithmetic overflow, invalid configuration and persistence
failures throw `TradingError` with an explicit code. They publish no partial state.
`submit` returns the assigned order ID even for rejection. Accepted orders can
immediately finish or cancel; inspect their resulting status and filled quantity.

Supply at most one quote and one valuation per OSI per `on_quotes` batch. The whole
batch is installed before any matching or risk decision. Unknown symbols, future
or negative data times, and duplicate symbols in one batch reject the whole batch.
An empty batch advances time, expiry, DAY cancellation, freshness and loss checks.
A thrown invalid input does not advance time; the caller should send a valid clock
batch when it still needs boundaries processed.

Each quote has a strictly increasing **per-contract observation number**, independent
of price changes. Repeated or older IDs and backward quote times are ignored;
they cannot replenish liquidity or marks. Same prices with a genuinely new ID can
replenish liquidity. A locally repeated cached poll must keep its old ID. Older
valuation timestamps are ignored; same-time valuation revisions are allowed.
Invalid/nonfinite valuation frames are stored as explicitly invalid, not zero risk.

Snapshots own account, marked positions, open orders, order/fill history, risk and
utilisation, scenarios, quality flags and account version. Previous publications
remain unchanged. `snapshot_json()` is a stable diagnostic serialization: money is
integer micro-dollars and enum ordinals (except string reason codes) are internal
schema details, **not a proposed HTTP wire format**. A session without a journal is
an explicitly ephemeral simulation. Production integration should provide a sink.

## Instruments and prices

Only standard European cash-settled index contracts with multiplier 100 are accepted:
SPX/SPXW, XSP, NDX/NDXP, RUT/RUTW, XND, MRUT, DJX, VIX/VIXW. The underlying must
match the root. American exercise is rejected with `AMERICAN_UNSUPPORTED` before
other eligibility checks. Adjusted deliverables and other roots are rejected.
Strikes must be positive, representable in OSI's thousandths and eight digits;
expiry dates must be valid in OSI's 2000–2099 range. Explicit AM/PM terms from the
known definition determine expiry; conflicting terms cannot replace a definition.

All booked money is signed int64 **micro-dollars**. `Money::parse("4.60")` is exactly
4,600,000. Input permits a sign, integral digits, and up to six fractional digits;
it rejects whitespace, exponent notation, empty fractions and excess precision.
Formatting preserves meaningful micro-dollars and prints at least two decimals.
`from_double` is an adapter boundary conversion, nearest micro-dollar, ties away
from zero. Prefer exact decimal parsing whenever a source provides decimal text.
Every addition, subtraction, multiplication, division allocation and conversion is
checked; overflow raises `ARITHMETIC_OVERFLOW`, never wraps.

Limit prices are positive multiples of this **v1 simulation tick policy**:

| Root | Price below $3.00 | Price at or above $3.00 |
| --- | ---: | ---: |
| SPX, SPXW, NDX, NDXP, RUT, RUTW | $0.05 | $0.10 |
| XSP, MRUT | $0.01 | $0.05 |
| XND, DJX, VIX, VIXW | $0.01 | $0.01 |

This table implements the requested policy; it is not a full exchange order-routing
specification. Observed fill prices need not themselves be on the limit-order tick.

## Orders and quote matching

Orders have buy/sell side, a positive integer contract count, a client ID and one
of market/IOC or limit/DAY/IOC. Market/DAY, market with a limit, and limit without
a positive price reject. Client IDs cannot be reused, even after a rejected order;
HTTP retry/idempotency semantics belong to the integration layer.

Buy execution uses ask; sell execution uses bid. A limit executes only if the far
side is no worse than its limit, including equality, and receives the observed far
side (possible price improvement). Market orders never sweep undisplayed depth.
Unfilled DAY limits rest; unfilled IOC quantity cancels with `IOC_REMAINDER`.
`filled_quantity` remains separate from terminal state: cancelled orders may have
fills. Each partial fill charges `quantity * fee_per_contract` (default $0.65).

Both positive prices and both positive integer sizes are required; crossed,
one-sided, missing and zero-size books supply **no liquidity**. Locked positive
books are accepted. Displayed size is an independent bid/ask budget per
(contract, observation), consumed across all paper orders. The same observation
never refills that budget, including after recovery. Each new observation refreshes
it once. Fractional provider sizes must be converted conservatively by the caller.

Resting buy limits cross when ask <= limit; resting sell limits cross when bid >=
limit, on a new valid observation timestamped at/after acceptance. A newly
submitted order may execute against the latest cached quote if it is still fresh.
For each OSI and side, better limit price wins,
then acceptance sequence (order ID). Buy and sell budgets are separate; buys are
processed first for deterministic cross-side risk effects. OSIs are processed in
lexical order after atomic installation of the entire batch. Submissions also
respect existing better orders when sharing the current budget. There is no queue
position, trade-through, slippage or hidden-liquidity simulation in v1.

Acceptance and execution are restricted to `md::trading_session(root, time)`'s
**regular** session. Each DAY order retains that acceptance day's regular session
end. The current calendar gives these index roots 16:15 ET, or 13:15 on early-close
days. Contract expiry can be earlier and takes precedence (e.g. PM expiry 16:00).
Boundaries process on the first command at/after them, before possible fills.
Outside 2025–2028 the existing calendar only knows weekdays; no extra calendar is
introduced here. Extended/global/curb execution is deferred.

## Accounting, marks and equity

Let `q` be signed contracts, `M = 100`, `p` fill premium per unit and `f` the fill fee:

```text
cash change = -q * M * p - f
```

Basis is **signed remaining acquisition notional**, excluding fees. Same-direction
adds add signed notional. Opposite fills allocate the existing signed basis in
proportion to the closed contracts, round nearest micro-dollar with ties away,
and keep the rounding residue in the remaining position. Gross realised P&L is
signed close proceeds minus allocated signed basis. A final close releases the
entire residue. A reversal closes first and opens the excess at the fill price.
Closed positions disappear; cumulative realised P&L and fees remain in the account.
Position realised/fees describe the current open lifecycle.

```text
market value = q * M * mark
unrealised P&L = market value - signed basis
equity = cash + sum(market value)
equity = initial cash + realised P&L + unrealised P&L - fees
```

Fresh valid two-sided books establish a midpoint mark, rounded to the nearest
micro-dollar (a half micro-dollar rounds up). An invalid or stale observation never
replaces the last valid mark with zero. Snapshots retain mark time/age, the last-mark
equity estimate and `valuation_complete = false`. Execution fails closed if any
held position lacks a fresh mark. Awaiting-settlement positions retain their last
mark and are always incomplete. If no mark exists, market value/unrealised are null;
the equity field is only a partial estimate and must be read with its completeness
flag. Normal session fills always establish a mark first.

## Risk and kill switch

Valuations come from the caller's coherent strike-smile frame: spot delta/gamma,
vega per vol point, theta per calendar day, spot S, forward F, discount D, years T
and **the smile IV actually used for the Greeks**. All must be finite; S/F/D/IV must
be positive and T nonnegative. Quote and valuation max age default to 60 market-time
seconds, configured independently. Missing or stale portfolio/pending-order values
block new trading. Expired positions do not pretend to have live Greeks.

Per-underlying and aggregate exposure:

```text
dollar delta          = sum(q * M * delta * S)
dollar gamma per 1%    = sum(q * M * gamma * S^2 * 0.01)
vega per vol point    = sum(q * M * vega)
theta per day         = sum(q * M * theta)
```

Gamma here measures dollar hedge change, not second-order scenario P&L. No dealer
sign convention or exposure time floor is used.

For delta and vega independently, start at current position exposure and add each
open order's remaining signed exposure to either the positive or negative endpoint:

```text
reachable low  = current + sum(min(0, order exposure))
reachable high = current + sum(max(0, order exposure))
reserved risk  = max(abs(low), abs(high))
utilisation    = reserved risk / limit
```

This protects against any subset of pending orders filling. Opposing orders cannot
cancel each other's reservations. Check each underlying (optional overrides,
otherwise common defaults) and the aggregate. Zero limits are allowed; positive
exposure against zero reports the largest finite double utilisation and rejects.
Overflowing analytical exposures mark risk incomplete and block trading.

Pre-trade checks require a supported registered unexpired contract, regular session,
valid order/tick, max order quantity, valid fresh quote, price band, complete marks
and valuations, daily-loss allowance, exposure reservations and an unlatched kill
switch. Price protection is inclusive:

```text
abs(price - midpoint) <= max(absolute_band, relative_band * midpoint)
```

At acceptance, price is the limit or market far side. At fill time it is the actual
far side, allowing favorable moves without comparing a stale limit to the new mid.
Checks rerun against current state before each proposed fill. Fill projection also
includes spread and fees in daily loss. A failed fill check cancels the remaining
order with `RISK_CHANGED`, preserving the underlying reason in its message and
actual/limit/scope. Noncrossed resting orders wait; invalid quotes supply no fills.
Limit changes apply immediately, increment a revision, and cancel affected orders
in acceptance order when rechecks fail. Existing positions are never force-liquidated.

Daily loss is `max(0, start_of_day_equity - equity)` including marks and fees. A loss
**strictly greater than** the configured allowance trips the latch and cancels all
open orders **before matching** on that market batch. Manual `trip_kill` does the
same. New orders reject with `KILL_SWITCH`. `reset_kill` requires a nonblank reason,
records the reset, and immediately re-trips if the loss still breaches. A reset
cannot make stale data tradable. Settlement is still permitted while killed.

`roll_day` is an explicit command on a later New York date, requiring complete marked
equity. It first monitors the old daily baseline, then stores the new baseline.
Repeated same-day rollover rejects. The kill latch survives rollover and recovery.
There are no deposits/withdrawals, margin, buying power, cash-interest, reduce-only
exceptions or automatic liquidation in this version. Negative cash/short positions
are permitted subject to the stated limits; this is not a brokerage margin model.

## Scenarios

Default spot shocks: -10, -5, -2, -1, 0, +1, +2, +5, +10 percent.
Default volatility shocks: -5, 0, +5, +10 **vol points**. Rows are spot-major.

```text
S' = S * (1 + spot_percent / 100)
F' = F * (1 + spot_percent / 100)
sigma' = max(vol_floor, smile_IV + vol_points / 100)
D' = D; T' = T
P&L = sum(q * M * (Black76(F', K, T, sigma', D) - Black76(F, K, T, IV, D)))
```

Black-76 consumes the scaled forward; scaling spot together expresses the sticky
strike carry assumption. Default floor is 0.0001. Each cell flags any floor clamp.
Zero spot/vol shock is **exactly zero**, bypassing subtraction and floor artifacts.
P&L is analytical double dollars, not booked cash. Invalid grids throw; axes are
bounded to 101 values each, spot > -100% and <= +1000%, absolute vol shock <= 1000
points. Missing/expired/nonfinite pricing inputs flag the whole grid incomplete;
its numeric placeholders are zero and must not be displayed as measured P&L.

## Expiry and explicit settlement

At `OptionContract::expiry_time()` orders cancel and open positions become
`awaiting_settlement`. No underlying quote is automatically taken as settlement.
The caller supplies the authoritative reference with `settle(OSI, value, time)`
after expiry. The next batch must obtain PM closing prints or explicitly imported
AM settlement values, and record their provenance outside this core.

```text
intrinsic = max(0, omega * (settlement_reference - strike))
cash payment = q * M * intrinsic
realised settlement P&L = cash payment - signed remaining basis
```

Settlement removes the position, charges no fee and is exactly once per OSI. OTM
options pay zero and release their entire basis into realised P&L. Negative
references, premature settlement, missing positions or unknown contracts reject.
The existing expiry API does not model the prior-day last-trading cutoff for all
AM products; this v1 uses the requested expiry boundary and regular-session policy.
Stock delivery, American exercise and assignment are deferred.

## Journal, recovery and failure handling

`FileJournal` creates a new exclusive single-writer POSIX regular file or resumes
an existing verified one. Each line is one **atomic reducer transaction**. Its
payload contains ordered typed outcomes (`order_accepted`, `order_rejected`,
`fill`, `cancel`, `definition`, `limit_change`, `kill_trip`, `kill_reset`,
`settlement`, `day_rollover`, `session_start`), the resulting reducer state, and the
published snapshot. Grouping a partial fill and IOC cancellation in one committed
line prevents recovery from exposing half a command. The top-level type names the
command (`submit`, `market`, etc.); outcomes are in `payload.events`.

Every record has exactly `seq`, `time`, `type`, `payload`, `prev_hash`, `hash`.
Sequence starts at 1; the genesis previous hash is 64 ASCII zeroes. Payload schema
is 1 and tick policy is `index-v1`. The canonical encoding is compact nlohmann JSON
3.12 serialization: recursively lexicographically sorted object keys, array order
preserved, UTF-8 strings, integer money, round-trip decimal doubles, no whitespace.
Hash is lowercase hex SHA-256 (OpenSSL EVP) over the canonical entire record with
**only the `hash` member omitted**. Newline is not hashed. Verification also requires
the original line to equal canonical serialization, detecting duplicate keys and
whitespace alterations. Hashes, sequence and monotone time are verified.

The complete line is written and `fsync` succeeds before a transition is published.
I/O failure throws `JOURNAL_IO`, leaves the prior account/orders visible, sets the
snapshot's `journal_failed` flag, and refuses every subsequent command. The disk
outcome can be indeterminate after a failed write/sync: **stop trading and recover**;
do not retry against the old in-memory account. A custom `Journal` must honor the
same all-or-error durability contract. File creation uses restrictive permissions;
the hosting application should durably provision the containing directory.

Recovery verifies the chain and restores **recorded outcomes/state**, including
cash, basis residues, liquidity budgets, observation high-water marks, definitions,
orders, fills, limits, kill state, daily baseline and the exact published snapshot.
It never reruns market matching or reprices a recovered snapshot. A resumed sink
must match the recovered head and sequence exactly. Subsequent commands calculate
normally from recovered state.

A non-newline-terminated final suffix is reported by
`JournalRecovery::truncated_final_line` and ignored, even if it looks like complete
JSON. Broken newline-terminated records fail loudly. Read-only recovery of the
verified prefix is possible, but `resume` refuses a torn suffix. The operator must
explicitly preserve/export the verified prefix before resuming; no recovery API
silently truncates the file. Persist the reported head independently and pass it
to `read`/`verify_journal` to detect removal of complete trailing records. An
unanchored hash chain cannot detect wholesale history replacement or a clean tail
truncation.

Full outcome checkpoints favor auditability and deterministic recovery over space
and throughput. All v1 orders/fills remain in memory and the snapshot's `recent_*`
arrays; there is no retention cap or compaction. This is appropriate for this core
batch; long-running production use will need bounded history and checkpoint policy.
Identical inputs give identical output on the same build. Exact monetary results
are portable; floating-point analytics are not promised bit-identical across
compilers/architectures, although recovery restores the recorded doubles.

## Reason codes

| Code(s) | Meaning |
| --- | --- |
| `NONE` | Success |
| `INVALID_MONEY`, `ARITHMETIC_OVERFLOW` | Invalid decimal/value or checked arithmetic range exceeded |
| `AMERICAN_UNSUPPORTED`, `NONSTANDARD_UNSUPPORTED`, `ROOT_UNSUPPORTED` | Outside v1 instrument scope |
| `INVALID_CONTRACT`, `UNKNOWN_CONTRACT` | Invalid/conflicting terms, or missing resolved definition |
| `INVALID_ORDER`, `DUPLICATE_CLIENT_ID`, `INVALID_TICK` | Malformed order, reused key, invalid price increment |
| `INVALID_QUOTE`, `STALE_QUOTE`, `MISSING_VALUATION` | No executable book, stale/incomplete marks, missing/stale/invalid Greeks |
| `MAX_ORDER_CONTRACTS`, `PRICE_BAND` | Quantity or protected-price bound exceeded |
| `DELTA_LIMIT`, `VEGA_LIMIT` | Worst reachable exposure exceeds underlying/aggregate limit |
| `DAILY_LOSS`, `KILL_SWITCH` | Daily equity allowance breached, or kill latch active |
| `RISK_CHANGED` | Fill/limit-change recheck failed; original cause in message, numeric evidence retained |
| `IOC_REMAINDER`, `USER_CANCEL`, `DAY_END` | IOC remainder, explicit cancellation, acceptance-day session end |
| `SESSION_CLOSED`, `EXPIRED`, `AWAITING_SETTLEMENT` | Outside regular hours, expiry boundary, or pending settlement quality flag |
| `INVALID_SETTLEMENT`, `ALREADY_SETTLED` | Invalid/premature settlement or already settled OSI |
| `UNKNOWN_ORDER`, `ORDER_TERMINAL` | Invalid cancellation target or already finished order |
| `INVALID_LIMITS`, `INVALID_TIME`, `INVALID_SCENARIO`, `INVALID_REASON` | Invalid control/configuration input |
| `JOURNAL_IO`, `JOURNAL_CORRUPT` | Persistence stop condition or invalid/tampered recovery chain/schema |

## Tests and next batch

`tests/support/scripted_market.hpp` supplies reproducible contract definitions,
market times, observations, quotes, valuations and requests for reuse by engine/API
tests. `tests/trading/` covers checked decimal arithmetic; long/short accounting and
basis residues; all execution/budget/priority/clock rules; open-order risk ranges;
loss/kill controls; scenarios; settlement; deterministic journal round trips,
tampering, torn suffixes, exclusive writers and injected write failures.

Engine/HTTP/web integration, feed observation identity generation, authoritative
settlement sourcing, authentication, external idempotency, stock positions,
assignment, trade-through matching, attribution and margin are intentionally outside
this batch. No shared engine/provider/app/web source was changed.
