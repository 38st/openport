# Paper trading (v1)

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
| `session.hpp` | `TradingSession`, `CommandResult`, immutable `TradingSnapshot`, `PayoutQuote`, `payout_quote` |
| `evaluation.hpp` | `Evaluation`, `EvaluationDay`, `Payout`, `AttemptSummary`, `Closure`, `BuyingPower`, `naked_requirement`, `MarginLeg`, `margin_requirement` |
| `history.hpp` | `Lifecycle`, `lifecycles` (round trips rebuilt from fills and closures) |

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

Reducer commands are `define`, `submit`, `modify`, `cancel`, `cancel_all`,
`close_positions`, `on_quotes`, `set_limits`, `trip_kill`, `reset_kill`, `settle`,
`roll_day`, `reset_account`, `request_payout` and `annotate`. Every completed command, including
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
On an idle account (no positions, no open orders, the attempt started) time drives
none of these, so an empty batch is not a transaction: nothing is journaled, the
account version and snapshot time stay put, and the next transaction advances the
clock. Rollover is its own command and still happens on a new trading day. A thrown
invalid input does not advance time; the caller should send a valid clock batch when
it still needs boundaries processed.

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

Standard contracts with multiplier 100 are accepted in two families. European
cash-settled index options: SPX/SPXW, XSP, NDX/NDXP, RUT/RUTW, XND, MRUT, DJX,
VIX/VIXW. American equity, ETF and OEX options on any other root, e.g. SPY, QQQ, IWM,
AAPL. The underlying and exercise style must match the root's conventions: American
exercise on a European index root is `AMERICAN_UNSUPPORTED`, a European contract on any
other root is `ROOT_UNSUPPORTED`. Adjusted deliverables are rejected.

American options are simulated without early exercise or assignment, dividends, or
stock positions. A position held into expiry settles in cash at intrinsic value from
the settlement reference, as if exercised or assigned and closed at the closing print.
Evaluation plans close positions five minutes before expiry, so this only affects
accounts without an expiry cutoff. Greeks and scenarios use the analytics' European
Black-76 values at the de-Americanised smile IV.
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
| SPY, QQQ, IWM | $0.01 | $0.01 |
| OEX | $0.05 | $0.10 |
| Other equity and ETF roots | $0.01 | $0.05 |

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

Orders trade in the sessions `md::trading_session(root, time)` gives each product.
Every product has its **regular** session: 09:30 to 16:15 ET for index roots and SPY,
QQQ, IWM and DIA, 16:00 for other equity options, or 13:15 and 13:00 on early-close
days. SPX/SPXW, XSP, VIX/VIXW and RUT/RUTW options also trade in Cboe's **overnight**
(global trading hours) session, 20:15 to 09:25 ET, which belongs to the next trading
date, and in the **curb** session, 16:15 to 17:00 ET after a full day (see
[runtime notes](runtime.md#product-sessions-and-cboe-clocks)). As on Cboe, the overnight and curb
sessions take limit orders only; openport also leaves triggers and brackets out of
them, so they take plain limit orders, single or multi-leg, DAY or IOC. Anything else
rejects with `LIMIT_ONLY`, and orders between sessions with `SESSION_CLOSED`. A DAY
order lasts the session it was accepted in: an overnight order ends at 09:25 with
`DAY_END` and does not carry into the regular session. Bracket exits, triggered
orders and the account's own closing orders (liquidation, expiry close) act in the
regular session only, and flattening sends market orders, so it rejects outside it.
AM-settled series stop trading at the regular close of the business day before their
expiry, so no curb or overnight session trades them then (`SESSION_CLOSED`); PM
series trade the overnight session of their expiry date. Contract expiry can be
earlier than a session end and takes precedence (e.g. PM expiry 16:00). Boundaries
process on the first command at/after them, before possible fills. Outside
2025–2028 the calendar only knows weekdays.

The engine also compares each underlying's market-data time with wall-clock time. A
healthy feed shows the market as it was the provider's stated delay ago, stopping at
the end of a session. When that moment is in one of the product's sessions, or data
stopped inside a session that should since have ended, a lag **greater than
`max_quote_age`** beyond it rejects new orders with `FEED_STALLED`, including the lag
behind the wall clock in the message (for example, "SPX quotes are 10h 20m behind the
market; the feed appears to have stalled"). A feed that rightly shows a closed market
returns `SESSION_CLOSED`, naming why (between sessions, a weekend or a holiday): for
the first 15 minutes of a session, a healthy 15-minute delayed feed still shows the
market before it. The simulation clock stays at market time. Resting orders do not
fill from stale data and are not cancelled merely because the feed stalls; they
remain subject to normal market-time DAY/expiry, risk and explicit cancellation rules.

## Conditional and bracket orders

Any order may carry a **trigger** `{source, direction, level}`. It is accepted with the
normal pre-trade checks, then rests as `Armed`: open, cancellable, reserving exposure and
buying power, but never matched. Option triggers compare the order's executable side
(ask for buys, bid for sells) from a fresh book; underlying triggers compare spot from
the contract's fresh valuation. Levels are inclusive (`at_or_below`, `at_or_above`), and
missing or stale data never triggers. Triggers are checked after each market batch's
resting orders match, and at submission, only during the contract's regular session.
A reached order is activated: entries rerun every pre-trade check, while bracket exits
need only an executable book. Stale data or a closed session keeps it armed; any other
failure cancels it with `RISK_CHANGED`. It then trades like any order (market orders
IOC). Armed orders are good until the contract expires.

A **bracket** `{stop_loss, take_profit}` on an entry creates exits as the entry fills.
Each exit takes exactly one of a trigger (a stop: market IOC when reached) or a limit
price (a resting take-profit). Exits take the opposite side and are sized to the entry's
filled quantity; later entry fills grow them. Client IDs are the entry's with `:stop` or
`:target`. The two exits are linked: the first fill of one cancels the other with
`OCO_FILLED`. Exits never exceed the position they protect: they shrink when it shrinks
and are cancelled with `POSITION_CLOSED` once it is flat, so they never open a
position. Because they only reduce risk, they execute like system orders, without the
price band or loss projection, and good-until-expiry exits wait for the next regular
session. A bracket pair counts once in reachable exposure and buying power (fees only);
exits never count against buy-only sells, so a manual close is always possible.

## Multi-leg orders

An order with **legs** trades two to four contracts together: each leg names a
registered contract, a side and a ratio from 1 to 10, all on one underlying (expiries may
differ, so calendars and diagonals are allowed). The order has no symbol, side, trigger
or bracket; its `quantity` counts units, and `limit_price` is the net per unit, positive
for a debit paid at most and negative for a credit received at least (zero is even).
Market orders are IOC as usual. The net must be a multiple of the smallest lower-tier
tick among the legs ($0.05 for SPX-class roots, $0.01 for XSP and equities).

Checks run per leg where they apply: registration, expiry, the session, a fresh
executable book, and `units * ratio` within `max_order_contracts`. The net price must lie
in the price band around the net mid, where the band is as wide as the one for the legs'
gross premium (`max(absolute, relative * sum of ratio * mid)`). Buy-only plans reject
multi-leg orders (`BUY_ONLY`), a leg inside the pre-expiry cutoff rejects the order
(`EXPIRY_CUTOFF`), and daily loss, exposure and buying power apply to the whole order.

A multi-leg order fills **all legs together**, in ratio, when the net at the far sides
(asks for bought legs, bids for sold legs) is at or below its limit. Each leg fills at
its own far side, so the net may improve on the limit, and units are bounded by every
leg's remaining displayed size. Multi-leg orders match after single-leg orders on the
same books, in acceptance order, and only on quotes newer than their acceptance (except
at submission). Each leg's fill is recorded under the order's ID; `filled_notional` and
the average fill are the net per unit. The projected fill is checked for daily loss and,
when any leg opens contracts, buying power, exactly like a single-leg fill. A working
multi-leg order counts as one pending exposure (its legs summed), and it is cancelled at
its earliest leg's expiry or its session end like any DAY order.

## Changing, cancelling and flattening

A resting order changes in place (`modify`): a DAY limit order, an armed order or a
bracket exit. It keeps its ID, its fills and its place among equal prices. The new
quantity counts filled contracts too and must exceed them; the limit price applies
to limit orders (a multi-leg order's signed net); the trigger level to armed orders
with a trigger. The changed order takes every pre-trade check a new one would, with
its own reservation released first, and a failure leaves it exactly as it was. A
limit that becomes marketable trades at once against the cached fresh quote, and an
armed order whose new level is reached activates in the regular session. Bracket
exits change only their level or take-profit price (positive, on the tier tick);
their size follows the position. The engine applies a new order's feed gate
(`FEED_STALLED`) before a change, because a change can trade.

`cancel_all` cancels every open order, armed ones and bracket exits included, or
only one underlying's. `close_positions` flattens the account or one underlying: it
cancels the open orders in scope, then closes each unexpired position in scope with
a market IOC order at the displayed quote, short positions first so buying one back
never uncovers another leg. These are the trader's own orders (client IDs
`openport-close-{version}-{n}`) and take the normal checks; one the rules refuse is
recorded as rejected with its reason and the others still go. Expired positions wait
for settlement.

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
held position lacks a fresh mark. Freshness (`max_quote_age`, and `max_valuation_age`
for Greeks) is measured at the market time while the contract's market is open, and at
its last session's end while it is closed, so a position in a closed market keeps its
closing mark: an SPY position held overnight does not block SPX trading in the
overnight session, and the day rolls over on it. Awaiting-settlement positions retain their last
mark and are always incomplete. If no mark exists, market value/unrealised are null;
the equity field is only a partial estimate and must be read with its completeness
flag. Normal session fills always establish a mark first.

## P&L by Greek

The snapshot explains the day's P&L by the Greeks (`attribution`, and
`attributions` per contract held or traded today). Each stretch a position is held
at one size starts from its mark and valuation (`Reference`): at a fill, at the day's
rollover, or when the position opens. Its change in value to the mark now is split
by the Greeks at its start, per unit times quantity times the multiplier:

```text
delta = delta0 * (S - S0)            gamma = 0.5 * gamma0 * (S - S0)^2
vega  = vega0 * (iv - iv0) * 100     theta = theta0 * (T0 - T) * 365
other = (mark - mark0) - delta - gamma - vega - theta
```

with spot `S`, the strike's smile volatility `iv` and years to expiry `T` from the
valuations at either end. Without valid valuations at both ends it is all `other`. A
fill ends the stretch at the mark and starts one at the new size; the spread paid
against the mark and the fee are `costs`. Settlement ends the last stretch at
intrinsic value, at expiry, with the settlement as the underlying's price and the
volatility unchanged. So the parts add up to the day's P&L exactly (up to floating
point): equity less the day's baseline. Rollover records the finished day's parts in
its `EvaluationDay` and starts every stretch again from the closing marks; a reset
starts afresh. Stretches and finished parts are state, journaled and recovered; a
position held from before an upgrade joins at its next fill or rollover. The parts
are analytic dollars, not accounting: vega is per volatility point and theta per
calendar day, as in the valuations.

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

Pre-trade checks require a supported registered unexpired contract, an open session that takes the order,
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
in acceptance order when rechecks fail. Limits never force-liquidate positions; only
account rules do (see Account rules).

Daily loss is `max(0, start_of_day_equity - equity)` including marks and fees. A loss
**strictly greater than** the configured allowance trips the latch and cancels all
open orders **before matching** on that market batch. Manual `trip_kill` does the
same. New orders reject with `KILL_SWITCH`. `reset_kill` requires a nonblank reason,
records the reset, and immediately re-trips if the loss still breaches. A reset
cannot make stale data tradable. Settlement is still permitted while killed.

`roll_day` is an explicit command on a later trading date, requiring complete marked
equity. A trading date (`md::trading_date`) is a business day's New York date until
17:00 ET, when its last session (curb) ends; after that, and over weekends and
holidays, it is the next business day, whose overnight session opens that evening. So
an overnight trade counts toward the day it trades for, and a day's close is the last
marked equity before 17:00. Rollover first monitors the old daily baseline, then
stores the new baseline. Repeated same-day rollover rejects. The kill latch survives rollover and recovery.
There are no deposits/withdrawals, cash interest or reduce-only exceptions. Without
the `buying_power` rule, negative cash and short positions are permitted subject to
the stated limits. With it, the naked-option requirement below applies; neither is a
full brokerage margin model.

## Account rules and evaluations

`SessionConfig::rules` (`AccountRules`) turns the account into an evaluation. The
defaults describe the plain paper account above: no target, no drawdown floor, any
side, no buying-power check. All rule money is exact.

| Rule | Effect |
| --- | --- |
| `profit_target` | Pass when equity reaches starting balance + target (zero disables) |
| `max_drawdown` | Fail when equity touches peak − drawdown (zero disables) |
| `drawdown_mode` | `Intraday`: the peak follows every fully marked equity high. `EndOfDay`: the peak moves only at rollover, from the last fully marked equity observed on the finished date |
| `buy_only` | A sell must close contracts already held, counting working sells on the same contract; otherwise `BUY_ONLY` |
| `buying_power` | New orders and their fills must not take buying power below zero; otherwise `BUYING_POWER` |
| `expiry_cutoff` | From expiry − cutoff until expiry, working orders on held contracts cancel with `EXPIRY_CUTOFF`, positions are closed, and only closing orders are accepted |
| `phase` | `Evaluation` (default) or `Funded`; a funded account has no profit target and pays out under `payouts` |
| `lock_balance` | Once peak − drawdown reaches it, the floor stays there and stops trailing (zero disables) |
| `payouts` | Funded phase: qualifying days, withdrawal share, trader split, minimum and caps (see Funded accounts and payouts) |

Outcomes use **fully marked equity**: every position has a mark, fresh or not. A
position without any mark defers the decision rather than counting as zero. Every
transaction runs the monitor after its command and the daily-loss check: it records
the day's latest marked equity, ratchets an intraday peak, fails on `equity <= floor`
(the floor is breached by touching it) and otherwise passes on `equity >= target`.
Breaches are checked on every transaction in both modes; the mode only controls when
the floor rises. The decision is sticky for the attempt: open user orders cancel with
`EVALUATION_CLOSED`, new user orders reject with it, and every position is liquidated.

**System orders** perform liquidation and expiry auto-close: market IOC orders with
`system = true` and client IDs `system:drawdown:N`, `system:target:N` or
`system:expiry:N`. They need a registered unexpired contract, the regular session and
a fresh executable book with displayed size on the closing side. They skip the kill
latch, price band, daily-loss, exposure, rule and buying-power checks because they
only reduce risk. Without executable liquidity nothing is recorded; the monitor retries
on later transactions until the account is flat, so system orders never accumulate.

**Buying power** is cash less the positions' margin requirement less working-order
reservations. Long premium is paid in full. A naked short option holds its buy-back
value (last mark, or its entry credit without one) plus the naked requirement
`100 * max(20% of spot - OTM amount, 10% of spot for calls or of strike for puts)`,
with the strike standing in for a missing spot. `margin_requirement` nets spreads. A
short pairs with a long of the same type on the same underlying that expires with it or
later, as a vertical: a put long below or a call long above its short costs the width,
one at or beyond it nothing, no pair costs more than naked, and unpaired shorts are
naked. The most exposed shorts take the most protective longs first. Positions that
expire together may instead need their worst loss at that expiry, when no net short
calls make it unbounded. Each underlying needs the least of pairing across expiries and
taking each expiry on its own (the lesser of its verticals and worst loss). So a credit
spread holds its width, an iron condor one wing, a long butterfly nothing, a calendar
nothing beyond its debit, a diagonal the strike difference when its long is further out
of the money, and a short strangle both naked requirements. A long that expires before
its short does not cover it. (European puts can trade below intrinsic value before
expiry; the pairing ignores that.)

Each working order reserves what filling it now would cost: its fees, plus the change
in the positions' margin requirement, plus the premium it pays less the premium it
receives, and never less than its fees (new shorts are valued at the order's price, or
at their marks for a multi-leg order). So an opening buy reserves its premium, a naked
sell its naked requirement (its credit covers the buy-back value), a sell against a held
long (legging into a spread) the width less its credit, a multi-leg credit spread its
width less its credit, and a closing order only its fees. Single-leg orders see the
positions less the contracts that earlier orders, in acceptance order, already claim to
close, so two sells cannot both claim the same long; otherwise each order is measured
against the held positions alone.

An order or a fill that would reduce free buying power (cash less the positions'
requirement) must leave available buying power nonnegative, otherwise `BUYING_POWER`
(`RISK_CHANGED` at a fill). One that frees buying power is always allowed: closing a
position, buying back a short and buying protection work even when buying power is
negative. Selling the long leg of a spread alone needs enough buying power to carry the
short it uncovers; buy the short back first, or close both together as one multi-leg
order (the terminal's Positions page picks positions and does this with Close together). When legging in, a short sold before its long is naked until the long is bought.
Fills recheck against the projected ledger and cancel the remainder with `RISK_CHANGED`.

`reset_account(initial_cash, rules, reason, time)` starts a new attempt. It cancels
working orders with `ACCOUNT_RESET`, records each open position as a `Reset` closure
at its last mark (average price without one; no fill, no fee), archives an
`AttemptSummary`, restores cash, clears the kill latch and applies the new rules. The
order and fill history is kept; `Evaluation::first_order/first_fill` mark where the
attempt begins. Settlements are also recorded as closures.

`Evaluation` carries the attempt number, start time, starting balance, peak, floor,
status and decision, plus one `EvaluationDay` per finished trading date (open and
close equity, peak and floor after that day's ratchet, net realised P&L after fees,
and whether it qualified toward a payout), appended at `roll_day`.

### Funded accounts and payouts

A funded account is an attempt whose rules have `phase = Funded`: no profit target
(`validate_rules` rejects one), the usual drawdown floor, and usually a `lock_balance`
equal to the starting balance so the floor stops trailing there. Touching the floor
fails the account like an evaluation; it stays closed until a reset. `PayoutRules`:

| Field | Meaning |
| --- | --- |
| `qualifying_profit` | A finished day qualifies with at least this much net realised profit (realised P&L less fees, from rollover to rollover) |
| `qualifying_days` | Qualifying days needed since the previous payout; at least 1 on a funded account |
| `withdrawal_percent` | Share of profit above the starting balance one payout may take |
| `split_percent` | The trader's share of each payout |
| `minimum` | Smallest payout |
| `caps` | Largest payout by payout number; the last cap repeats; empty is uncapped |

Qualifying days need not be consecutive. Each finished day counts once, toward the
cycle in progress when it rolls over; a payout resets the count, so the trading day of
the request (`Payout::day`) and later days count toward the next payout.
`payout_quote(snapshot, rules)` reports the standing: the next payout
number, the qualifying days, profit, the withdrawable share (whole cents), the cap,
the maximum and the trader's share at the maximum, and `blocked`, the first unmet
requirement in this order: funded phase (`PAYOUT_UNAVAILABLE`), an active account
(`PAYOUT_UNAVAILABLE`), no positions or open orders including armed ones
(`PAYOUT_NOT_ELIGIBLE`), the qualifying days (`PAYOUT_NOT_ELIGIBLE` with actual and
limit), and a maximum of at least the minimum (`PAYOUT_NOT_ELIGIBLE`). The maximum is
`min(withdrawable, cap)`; with a locked floor it also leaves equity at least a cent
above the floor.

`request_payout(amount, time)` takes a positive whole-cent amount (otherwise it
throws `INVALID_PAYOUT`), returns `blocked` when set, and rejects amounts outside
`[minimum, maximum]` with `INVALID_PAYOUT`. It then withdraws the cash (realised P&L is
unchanged) and records a `Payout` (number, time, trading day, amount, trader share,
equity before the withdrawal). A withdrawal is not a loss: the daily-loss baseline, the
open and close equity of the trading day in progress (even when requested after its
date ends and before rollover) and an unlocked peak and floor all move down by the
amount, so the day's P&L and the drawdown room are unchanged and an end-of-day ratchet
compares closes net of it; a locked floor stays where it is.

The web terminal hides funded plans and the Payouts page (this simulator funds no one)
unless the account is already funded; `showFundedAccounts` in `web/src/lib/features.ts`
offers them again. The server offers a funded preset for each evaluation preset
(`funded-intraday-25k` and so on). A reset into one requires that the current attempt passed the evaluation
it names, otherwise `PLAN_LOCKED`; `--plan` can start a new journal on one directly,
and custom rules may set `phase` freely. Preset parameters are this project's own,
modelled on common prop-firm terms: the evaluation's drawdown and strategy rules, the
floor locking at the starting balance, 8 qualifying days of $100, $150 or $200 (25K,
50K, 100K), up to 50% of profit per payout, 80% to the trader, a minimum of 1% of the
balance and caps of 2%, 3%, 4% and then 6% of the balance for payouts 1, 2, 3 and 4+.

`history.hpp`'s `lifecycles(fills, closures, contracts)` rebuilds round trips from flat
to flat. Each replays its own fills through a fresh `Ledger`, so realised P&L uses the
account's basis allocation and rounding exactly; a reversing fill closes one lifecycle
and opens the next at the same price with its fee split pro rata.

## Trade notes and tags

`annotate(trade, note, tags, time)` records the trader's note and tags on a trade,
named by the fill that opened it (the trades view's `id`). The note is trimmed and at
most 2,000 bytes of UTF-8 text, keeping newlines and tabs; up to eight tags of 1 to 32
bytes without commas or control characters are trimmed, lowercased and kept once
each. Text past these limits throws `INVALID_NOTE`; a fill that opens no trade returns
`UNKNOWN_TRADE`. An empty note without tags clears them. Notes are journaled
(`trade_annotated`) like any command, so they recover with the account and stay with
its history across attempts, and they are allowed whatever the account's state or
session. The snapshot's `annotations` maps each trade's ID to its note, tags and the
time it last changed.

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
after expiry. The hosting engine obtains PM closing prints or explicitly imported
AM settlement values, and records their provenance outside this core.

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
`settlement`, `day_rollover`, `session_start`) and the resulting reducer state, whole
or as its change from the record before. The published snapshot is derived from the
state, so it is not recorded. Grouping a partial fill and IOC cancellation in one committed
line prevents recovery from exposing half a command. The top-level type names the
command (`submit`, `market`, etc.); outcomes are in `payload.events`. Empty market
batches on an idle account are not transactions, so a flat account without working
orders adds no records while the feed polls; with positions or open orders, every
batch is recorded because time moves their rules and marks.

Creation and resume acquire a non-blocking exclusive advisory `flock(LOCK_EX |
LOCK_NB)` on the journal file before reading or writing its contents, held for the
session's lifetime and released when the journal closes on destruction (including
engine shutdown). `flock` is used instead of process-owned `fcntl` record locks so
separate opens in the same process also conflict, and closing a recovery reader
does not release the writer's lock. Another owner causes `JOURNAL_LOCKED` without
reading or changing the journal: analytics continue, paper trading is disabled,
and all trading writes return 503 `TRADING_UNAVAILABLE`. `/api/status`'s
`trading.reason` and the daemon's startup diagnostic name the path and say it is
in use by another openportd; use `--paper-journal` to choose another file or
`--no-paper`. All writers must honor the advisory lock; do not replace or unlink
an active journal.

Every record has exactly `seq`, `time`, `type`, `payload`, `prev_hash`, `hash`.
Sequence starts at 1; the genesis previous hash is 64 ASCII zeroes. New payloads use
schema 3 and tick policy `v2` (the `index-v1` table plus equity and ETF classes): the
`events`, the command's `decision`, and either `state`, the whole reducer state (a
checkpoint), or `delta`, its change from the previous record's state. A delta is a
tree of nodes: `{"v": value}` replaces a value, `{"o": {key: node}, "d": [keys]}`
changes some of an object's keys and removes others, and `{"a": {"index": node},
"n": length}` changes some of an array's elements and sets its length, new elements
arriving whole. The first record is a checkpoint, as are the first after every
recovery, one in every 1,000 records, and any record whose change is more than half
the size of the last checkpoint. Recovery rebuilds each record's state from the one
before and checks it against the record's sequence and time. Order, fill and closure
history therefore costs a record only what the transaction added: records stay a few
hundred bytes however long an account trades, where schema 2 wrote the whole state
and snapshot every time (a 14,063-record journal of mostly idle polls shrank from
60.6 MB to 5.4 MB, and its startup from 5.8 s to 0.6 s).

Schema 2 added `config.rules`, the evaluation, attempts and closures to the state and
snapshot, and `system` to orders; it also records `evaluation_passed`,
`evaluation_failed`, `evaluation_day`, `account_reset` and `payout` outcomes. Later
schema 2 fields (conditional and bracket orders, the funded phase, payout rules and
records, qualifying days) default when absent, so earlier schema 2 journals recover
unchanged; multi-leg orders record their `legs`. Recovery reads schema 1 and 2
records, which hold the whole state and snapshot, in any mix with schema 3. Schema 1
journals keep their original keys required and default the added ones, and the
evaluation starts from the first record with the recorded starting cash. Resumed
schema 1 and 2 journals continue with schema 3 records; builds from before schema 3
refuse them rather than guess. The canonical encoding is compact nlohmann JSON
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
orders, fills, limits, kill state and daily baseline, and the published snapshot:
recorded (schemas 1 and 2) or derived from the recovered state as the reducer
derived it (schema 3). It never reruns market matching or reprices from new data. A resumed sink
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

Every record's whole state stays recoverable, so the journal is a complete audit of
the account at each transaction. All v1 orders/fills remain in memory and the
snapshot's `recent_*` arrays; there is no retention cap, and a checkpoint grows with
the account's history, one per thousand records.

### Compacting older journals

Records written before schema 3 keep their whole states until rewritten. Stop
openportd, then run `openportd --compact-journals` (with the same `--paper-journal`
if you set one; in Docker, `docker run --rm -v openport:/var/lib/openport openport
--compact-journals` while the server's container is stopped): it rewrites the main journal and each account journal beside it
that has such records, and reports the sizes. A rewrite takes the writer's lock, so
a journal in use is left as it is with `JOURNAL_LOCKED`; keeps every transaction's
sequence, time, type, events and decision; reads each rewritten record back before
writing it; and must recover to the same account before it replaces the journal. A
final schema 1 record stays as it is, since recovery completes that journal's
evaluation from it. The original stays beside it as `FILE.bak` (then `.bak2` and so
on): move it back to undo, or delete it once you are satisfied. The rewrite has a
new hash chain, so replace any head you persisted to anchor the old one.
`TradingSession::expand` does the reverse, writing every state and snapshot whole
(schema 2), for audit tools and for returning a journal to an older build.
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
| `SESSION_CLOSED`, `EXPIRED`, `AWAITING_SETTLEMENT` | Outside the product's sessions (or an AM-settled series after its last regular close), expiry boundary, or pending settlement quality flag |
| `LIMIT_ONLY` | The overnight and curb sessions take plain limit orders: no market orders, triggers or brackets |
| `FEED_STALLED` | Market data lags what a healthy feed would show by more than `max_quote_age`; message includes the lag behind the wall clock |
| `INVALID_SETTLEMENT`, `ALREADY_SETTLED` | Invalid/premature settlement or already settled OSI |
| `UNKNOWN_ORDER`, `ORDER_TERMINAL` | Invalid cancellation target or already finished order |
| `INVALID_LIMITS`, `INVALID_TIME`, `INVALID_SCENARIO`, `INVALID_REASON` | Invalid control/configuration input |
| `JOURNAL_IO`, `JOURNAL_CORRUPT` | Persistence stop condition or invalid/tampered recovery chain/schema |
| `JOURNAL_LOCKED` | Journal already owned by another writer; analytics remain available |
| `EVALUATION_CLOSED` | The attempt passed or failed; reset to trade again |
| `BUYING_POWER`, `BUY_ONLY`, `EXPIRY_CUTOFF` | Account-rule rejections (see Account rules) |
| `ACCOUNT_RESET` | Working order cancelled by a reset |
| `INVALID_RULES` | Negative rule money, cutoff of a day or more, a plan name over 64 bytes, payout percentages outside 0-100 or nonpositive caps, or a funded phase with a profit target or no qualifying days |
| `OCO_FILLED`, `POSITION_CLOSED` | Bracket exit cancelled by its sibling's fill, or because its position closed |
| `PAYOUT_UNAVAILABLE`, `PAYOUT_NOT_ELIGIBLE`, `INVALID_PAYOUT` | Not a funded, active account; a payout requirement unmet; or an amount that is not whole cents or outside the minimum and maximum |
| `PLAN_LOCKED` | A funded preset was requested without first passing the evaluation that unlocks it |
| `INVALID_NOTE`, `UNKNOWN_TRADE` | A trade note or tag past its limits, or a note on a fill that opens no trade |

## Engine integration and HTTP API

Paper trading is enabled by default in `openportd`; `--no-paper` disables it and
reports `PAPER_DISABLED` in status.

### Accounts

The journal at `--paper-journal` is the **main** account. More named accounts live
in an `accounts` directory beside it, one journal each (`accounts/<id>.jsonl`, the
display name in `accounts/<id>.name`), recovered at startup in ID order. Every
account trades the same market at once: each receives the quotes and valuations for
its own positions, open orders and commands, marks, fills, settles, rolls its day
and applies its rules whichever account the terminal shows. An account whose
journal cannot open, or fails later, reports why in its status and refuses writes;
the others carry on.

`POST /api/accounts` takes a `name` (1 to 64 printable characters) and either a
preset `plan` or `initial_cash` and complete `rules`, and returns 201 with the new
account's `id`, a slug of the name made unique (`swing-50k`, `swing-50k-2`). Funded
plans are refused: they start by resetting an account that passed the evaluation.
Every other trading route acts on the main account, or on another given as
`account=<id>` in the query (reads and writes alike; a write takes no other query
parameter). An unknown account is 404 `UNKNOWN_ACCOUNT`; a malformed one is 400.
Without an accounts directory (`--paper-journal` empty in tests) the server keeps
one account and account creation returns 503.

### Commands and views

The engine thread alone owns every session. A bounded FIFO inbox (256 pending
commands) sequences writes, applies the drained market batch first, then applies
commands in ingress order. HTTP threads enqueue
and return; completions are posted onto the requesting Beast session executor.
A full inbox or stopping engine returns 503 `TRADING_UNAVAILABLE`.

Every referenced listed contract is registered before its first quote batch. Held
positions, open orders and pending-command symbols receive quotes from ChainBook
and valuations from the latest coherent analytics frame at the strike smile IV.
Sizes are floored to whole contracts. Observation numbers increase only for new
OptionQuote events and resume above recovered high-water marks. Cached analytics,
underlying prints and HTTP reads never replenish displayed option liquidity.
**The market queue coalesces a contract’s quotes within one drain, so a fleeting
cross can be missed on streaming feeds.** Underlying prints are retained in order
so settlement uses the first qualifying print.

Each market batch and command publishes an immutable trading view per account. GET
endpoints read that view without touching the reducer. `/api/status` and WebSocket
ticks include the main account's `trading: {enabled, reason, account_version,
kill_latched, write, fee_per_contract, initial_cash}` and `accounts: [{id, name,
trading}]` for every account. The fee and original session cash are exact money
strings (defaults `"0.65"` and `"100000.00"`), taken from the active/recovered session
configuration; `initial_cash` is not the current balance or daily equity baseline.
Versions are decimal strings and `write` is `open`, `token`, or `disabled`. Clients refetch
portfolio, orders and risk when the version changes. Chain option objects include
canonical padded `symbol`, whole `bid_size`/`ask_size` (null when unavailable),
`tradable` and `untradable_reason`, using the core eligibility policy.

Each `/api/status` and tick `underlyings[]` entry includes
`paper: {accepting: boolean, reason: code|null, message: string|null, session}`, where
`session` is the session new orders enter by the underlying's market-data clock
(`regular`, `global`, `curb` or `closed`; null before any data). This uses the
same session/feed check as new orders, the underlying's market time (including
persisted quotes after recovery), provider delay and the active session's
`max_quote_age`. An accepting entry has null reason and
message; missing market data reports `INVALID_QUOTE`. Contract eligibility, risk,
kill-switch and write-access checks still apply separately. The existing `session`
field continues to describe the wall-clock product session; just after 09:30 a
15-minute delayed feed still trades the overnight session, which `paper.session` shows.

The Journal page edits each trade's note and tags (a strategy's apply to each of its
legs), filters every panel by tag and reports P&L by tag. Alerts belong to the web
terminal alone: price alerts on an underlying and alerts on each new fill are kept in
the browser's local storage and run while the terminal is open, on the live feed only
(not a replay). They show on the page and, where the browser allows notifications, as
system notifications, with an optional chime. A price alert fires once, when the
feed's price reaches its level from the side it was set on.

The web ticket estimates fees using `fee_per_contract`; only older servers without
it expose a manual fee estimate. Ticket and Positions notices use `paper.message`,
and `paper.accepting: false` disables ticket submission. In the overnight and curb
sessions (by `paper.session`) the tickets offer limit orders only, without a condition
or bracket, and Close all is disabled because flattening sends market orders. For
older servers without `paper`, they fall back to the session-based notice and
submission gate.
Limit prices display cents, with buttons and arrow keys following the root's tier
tick table above (including downward steps across $3.00). Typed off-tick prices
still receive the server's `INVALID_TICK` reason. On wide screens the ticket docks
beside the chain; elsewhere it is a dialog. It names the strategy from the held
position (Long Call, Close Short Put...), sets the limit from Bid/Mid/Ask, says whether
the order is marketable at the far side or will rest, and estimates the buying-power
effect with the server's reservation rules. Buy-only plans and decided attempts block
submission with the reason.

After HTTP 400/403/404/409/422, the ticket shows the rejection details and **New order**,
which preserves form values and starts a fresh client ID. **Retry same order** keeps
the frozen request and ID only after a network failure, timeout or 503. Other
unexpected responses direct the user to check Positions and Orders. Results receive keyboard
focus at the top of the ticket.

| Endpoint | Request / response |
| --- | --- |
| `GET /api/portfolio` | Account cash, equity, daily baseline/P&L, realised/unrealised, fees, completeness/quality flags, marked positions and Greeks, and today's `attribution` (`delta`, `gamma`, `vega`, `theta`, `other`, `costs`, `total` in dollars) for the account and each position (null until the position's next fill or rollover) |
| `GET /api/orders?status=all` | All orders, newest first; `status=open` restricts to working, partially filled and armed orders |
| `POST /api/orders` | `client_order_id`, canonical `symbol`, `side` (`buy`/`sell`), `type` (`limit`/`market`), integer `quantity`, decimal-string `limit_price` for limits, `time_in_force` (`day`/`ioc`), optional `trigger` `{source: option\|underlying, direction: at_or_below\|at_or_above, level}` and `bracket` `{stop_loss?, take_profit?}` whose exits each take one of `trigger` or `limit_price`. A multi-leg order replaces `symbol` and `side` with `legs` (two to four `{symbol, side, ratio?}`, ratio default 1), takes no trigger or bracket, counts units in `quantity` and sets a signed net `limit_price` (negative for a credit); 201 returns version, order and its fills. Orders report `legs` (null for single-leg), with null `symbol` and `side` for multi-leg orders |
| `DELETE /api/orders/{id}` | No body; 200 returns version and resulting order |
| `PUT /api/orders/{id}` | Any of integer `quantity`, decimal-string `limit_price` and `trigger_level`; 200 returns version, the changed order and its fills (see [changing orders](#changing-cancelling-and-flattening)) |
| `POST /api/orders/cancel` | Optional `underlying`; cancels every open order, or that underlying's, and returns version and `cancelled_orders` |
| `POST /api/positions/close` | Optional `underlying`; cancels the open orders in scope and closes its positions at market, returning version, `cancelled_orders`, the closing `orders` (each with its status and reason) and their `fills` |
| `GET /api/fills` | Version and fills, newest first |
| `GET /api/risk` | Version, limits revision, limits, complete flag, daily loss, kill state, aggregate/underlying buckets and scenario matrices |
| `PUT /api/risk/limits` | `expected_revision` string and complete `limits` object; 200 returns the risk view, 409 if revision changed |
| `POST /api/risk/kill` | `action` (`trip`/`reset`) and nonblank `reason`; returns version, kill state and cancelled order IDs |
| `POST /api/settlements` | Canonical `symbol` and decimal-string `value` for an expired AM position; returns version and `position_closed` |
| `GET /api/account` | Rules (including `phase`, `lock_balance` and `payouts`), evaluation (attempt, status, starting balance, equity, `marked`, profit, peak, floor, `floor_locked`, drawdown buffer, target equity/remaining, decision, current day, finished `days[]` with `realised`, `qualifying` and `attribution`, `qualifying_days`, `cycle_started` and `payouts[]`), buying power, `payout` (the next payout's standing from `payout_quote`: `eligible`, `blocked`, number, flat/active, qualifying and required days, profit, withdrawable, cap, maximum, minimum, trader share and percentages; null outside the funded phase) and earlier `attempts[]`; absent rules give null floor/target |
| `GET /api/trades?status=open\|closed\|all&attempt=current\|all` | Round trips, newest first: direction, status, opened/closed/duration, quantities, average open/close, cost (entry premium), gross, fees, net, `return` (net / cost, closed only), mark/unrealised while open, `closure` (`settlement`/`reset`/null), fill IDs, attempt, and the trader's `note` (`""` for none) and `tags`. Defaults: all statuses of the current attempt |
| `PUT /api/trades/{id}/note` | Optional `note` string and `tags` array replace the trade's (see [trade notes](#trade-notes-and-tags)); an empty note with no tags clears them. Returns version, `trade`, `note` and `tags`; `UNKNOWN_TRADE` (404) if no trade opens with that fill, `INVALID_NOTE` (422) for text past the limits |
| `GET /api/plans` | Presets: `practice` (buying power only), `intraday-25k/50k/100k` (buy-only, 10% target, 5% intraday trailing), `eod-25k/50k/100k` (any side, 12% target, 6% end-of-day trailing) and their `funded-*` accounts (`unlocked_by` names the evaluation); evaluations and funded accounts auto-close five minutes before expiry |
| `POST /api/account/reset` | Nonblank `reason` plus either a preset `plan` ID, or `initial_cash` and complete `rules` (optional `phase`, `lock_balance`, and `payouts` required exactly when funded); returns the new account view. Funded presets need a passed matching evaluation (`PLAN_LOCKED`) |
| `POST /api/account/payout` | Decimal-string `amount` in whole cents; returns the account view with the recorded payout |
| `GET /api/accounts` | `accounts`: each account's `id`, `name`, `trading` status and `equity`, the main one first |
| `POST /api/accounts` | `name` and a preset `plan`, or `initial_cash` and `rules`; 201 returns the new account's `id`, `name`, version, plan and equity (see [accounts](#accounts)) |

Every route in this table except `/api/plans` and `/api/accounts` takes `account=ID`
in its query for an account other than the main one (see [accounts](#accounts)).
`/api/replay` and the routes under it serve a replay of a recording; see
[replaying in the terminal](runtime.md#replaying-in-the-terminal).

Rules JSON is `{plan, profit_target, max_drawdown, drawdown_mode, buy_only,
buying_power, expiry_cutoff_seconds}` with null money for a disabled target or
drawdown and `drawdown_mode` `intraday` or `end_of_day`. Portfolio adds
`buying_power: {available, reserved, short_requirement}`; orders add `origin`
(`user` or `system`), `status` `armed`, `trigger`, `triggered_at`, `bracket`, `role`
(`stop_loss`/`take_profit`/null), `parent`, `oco`, `stop_loss_order` and
`take_profit_order`; status and ticks add `trading.plan` and `trading.evaluation`
(`active`/`passed`/`failed`, null without a target or drawdown rule). `--plan ID`
chooses the rules for a new journal (default `practice`); `--paper-cash` then overrides
its starting balance. Recovery keeps the recorded rules.

Money is an exact decimal string, quantities are integers, IDs/versions are strings,
and timestamps use the same UTC ISO format as `as_of`. Analytical values may be
null. Position Greeks expose per-unit delta/gamma/vega/theta plus signed position
dollar exposures. Incomplete scenario grids contain null P&Ls, never partial sums.
Limits contain `max_order_contracts`, `price_band_absolute`, `price_band_relative`,
`aggregate` and `per_underlying` (`dollar_delta`, `vega`), `max_daily_loss`,
`max_quote_age_seconds` and `max_valuation_age_seconds`.

Unknown fields, duplicate JSON keys, missing required fields, wrong types and
noncanonical OSIs return 400 `INVALID_REQUEST`. Business rejections return 422
and remain recorded as rejected orders; unknown contracts/orders return 404,
terminal orders/reused client IDs return 409. Retries with a reused client ID
are conflicts, not automatic replays. Errors always have this shape:

```json
{"error":{"code":"DELTA_LIMIT","message":"...","actual":1250000,"limit":1000000,"scope":"SPX"}}
```

Unused `actual`, `limit` and `scope` are null. Rejected writes still consume their
client ID; GET orders shows their resulting rejection reason.

### Write protection

Every POST/PUT/DELETE under `/api/` uses the same protection. POST and PUT require
`Content-Type: application/json` (an optional media-type parameter is accepted);
DELETE has no body. The body limit remains 64 KiB. A present Origin must match Host
or an exact `--allowed-origin`; invalid or ambiguous security headers fail closed
with 403 `ORIGIN_REJECTED`. Non-browser clients may omit Origin.

`--write-token TOKEN` overrides `OPENPORT_WRITE_TOKEN`. When configured, all writes
require `Authorization: Bearer TOKEN`, checked with a constant-time digest comparison;
missing/incorrect credentials return 403 `WRITE_TOKEN_REQUIRED`. Without a token,
only loopback binds allow writes. Non-loopback binds return 403 `WRITE_DISABLED`.
Reads remain open. Use HTTPS at your reverse proxy for remote bearer credentials
and configure its public Origin with `--allowed-origin` when it rewrites Host.

### Durable startup and settlement sources

`--paper-journal PATH` defaults to `$HOME/.openport/paper-journal.jsonl`; containing
directories are created, and named accounts keep their journals in `accounts/` beside
it. `--paper-cash` defaults to `100000` and `--paper-fee` to
`0.65`. These seed new journals; recovery restores the recorded configuration.
Existing files are verified, exclusively locked and resumed. A corrupt, torn, locked
or unwritable journal disables its account's writes with 503 `TRADING_UNAVAILABLE`
and a status reason; other accounts carry on. They are never overwritten or silently replaced by an ephemeral
account. A runtime journal failure preserves the last committed account and
requires operator recovery. The Docker image journals in `/var/lib/openport`,
which is a declared volume; mount a persistent volume there.

At expiry, PM positions settle from the underlying’s **first positive finite last
print stamped at or after the expiry instant on the expiry date**, in provider
arrival order. This is the **provider’s closing print**, an approximation of the
official settlement value. Bid/ask midpoints and next-day prices are not substitutes.
If no such print arrives, the position stays awaiting settlement. AM positions
always wait for an explicit `/api/settlements` import, whose value must come from
the operator’s authoritative settlement source; PM imports are rejected. The
same journal transaction records the reference value, canonical definition and
integration `settlement_source`: `provider_closing_print` with provider and print
time, or `manual_am_import`. Preserve the official source used for an AM import
externally when an independent provenance audit is required. The first market
batch on a later trading date rolls the daily baseline once marks are complete;
the kill latch survives. All accounting uses effective market time, including
delayed feeds, rather than HTTP receipt time.

## Tests

`tests/support/scripted_market.hpp` supplies reproducible contract definitions,
market times, observations, quotes, valuations and requests for reuse by engine/API
tests. `tests/trading/` covers checked decimal arithmetic; long/short accounting and
basis residues; all execution/budget/priority/clock rules; open-order risk ranges;
loss/kill controls; scenarios; settlement; evaluation and funded-account rules;
conditional and bracket orders; multi-leg orders, margin and buying power; order
changes, cancel-all and flattening; deterministic journal round trips, tampering,
torn suffixes, exclusive writers and injected write failures; state deltas (including
randomized round trips), checkpoints, damaged deltas, mixed-schema recovery and
compaction; overnight and curb sessions; trade notes and tags. The CLI tests compact journals from earlier builds with `openportd`.

Engine and HTTP tests reuse that fixture for resting fills, cancellation, kill/limits,
JSON errors, write protection, restart recovery, AM/PM settlement, named accounts and
replays. Socket tests cover asynchronous POST/DELETE responses and shutdown of pending
commands. External idempotency, stock positions, assignment, trade-through matching,
attribution and portfolio margin remain outside v1.
