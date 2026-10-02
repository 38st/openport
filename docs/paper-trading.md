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
| `journal.hpp` | `Journal`, `FileJournal::create/read/resume/flush`, `verify_journal`, `JournalRecovery` |
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
`roll_day`, `reset_account`, `request_payout`, `annotate`, `exercise` and `trade_stock`. Every completed command, including
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
VIX/VIXW. American equity, ETF and OEX options on any other root, e.g. SPY, QQQ, IWM, DIA,
AAPL. The underlying and exercise style must match the root's conventions: American
exercise on a European index root is `AMERICAN_UNSUPPORTED`, a European contract on any
other root is `ROOT_UNSUPPORTED`. Adjusted deliverables are rejected.

American equity and ETF options deliver shares; OEX, like the European index roots,
settles in cash. Held into expiry, an equity or ETF option a cent or more in the money
at the settlement reference is exercised or assigned: it settles at intrinsic value,
and 100 shares a contract change hands at the settlement reference, bought by long
calls and short puts, sold by long puts and short calls. Together that costs the
strike, while the option's own trade records its real result and the shares' P&L is
their own move from there. `exercise(symbol, contracts, time)` exercises long, in-the-
money contracts early in the same way against the underlying's fresh price, so any
time value left is a cost; it takes the account's checks and, with the
`buying_power` rule, must fit within it.

A long held into expiry can take a **do-not-exercise instruction**:
`instruct_exercise(symbol, true, time)` on a long option makes
it expire worthless at its settlement, however far in the money, with no shares or cash
delivered (its settlement closure is at 0.00), as a broker's contrary exercise advice
does. `false` withdraws it. The instruction ends with the position: selling the
contracts, exercising them early, abandoning them or a reset clears it. It also
forfeits the cash settlement of an index long; a short cannot take an instruction
(`INVALID_ORDER`). Both disposal commands allow the kill switch but reject a
closed evaluation (`EVALUATION_CLOSED`).

**Early assignment** follows the market rather than a model: at each day rollover
(`roll_day`, overnight), holders exercise a short American equity or ETF option when
they did better exercising than holding it at the close, as exercise notices are due
that evening. That is when its closing mark is below its intrinsic value at the
underlying's close (a deep put whose time value is gone), or, for a call, when its
time value is less than a dividend going ex on the new day (from `--dividends`). The OCC allocates exercises to short positions at random, and
openport cannot know how many holders exercise, so each such contract is assigned
with even odds, drawn from the account's attempt, the contract and the date: a
position can be assigned in part, or not that night, and a replay assigns the same
contracts. Assigned contracts are bought back at intrinsic value
(`ClosureKind::Assignment`) and deliver 100 shares a contract at the underlying's
close (`StockSource::Assignment`), together the strike; the new day takes the
difference from the marks, and the shares' move from the close. Options expiring
that day settle instead.

**Dividends** come from a file or from Massive. `openportd --dividends FILE` reads
`SYMBOL,YYYY-MM-DD,AMOUNT` lines (the ex-date and dollars a share; blank lines, `#`
comments and a `symbol,...` header are skipped) with `parse_dividends`. `--dividends
massive` reads the stock and ETF symbols' cash dividends in US dollars from Massive's
dividends endpoint (`GET /stocks/v1/dividends`, in every stocks plan) with
`MASSIVE_API_KEY`, whichever provider supplies the quotes: those going ex from a month
back on, when the server starts and every six hours after, hourly while a symbol fails
(`providers::MassiveDividends`), and two distributions going ex together are paid as
one. New ones reach the live accounts, and replays started after them. The same
schedule supplies known cash payments to American option analytics, with revised
or removed payments refreshing analytics even without new quotes; see
[American analytics](american-analytics.md#known-cash-dividends).
`roll_day(time, dividends)` takes those going ex after the last trading date and
on or before the new one (`dividends_due`, so a server that was down
across an ex-date still pays it), after the night's assignments: shares held into the
ex-date receive `per_share * shares` in cash and realised P&L, and short shares pay
it, once per symbol and date. Each is a `DividendPayment` in
`TradingSnapshot::dividends`, today's P&L by Greek counts it as other, and the share
round trip holding the shares adds it to its net. The payment keeps the number of
stock fills before it (`after_stock_fill`), so the round trip takes it even when the
shares are sold at the rollover's own market time; a payment recorded before that
comes after the night's assignments and deliveries at its time and before any trade. Without a file, shares held through
an ex-date lose the dividend in price without the cash. Evaluation plans close
positions five minutes before their last trade, so expiry delivery only reaches
accounts without an expiry cutoff. Greeks and
scenarios use the analytics' European Black-76 values at the de-Americanised smile IV.

Delivered shares (`TradingSnapshot::stocks`) are marked at the underlying's price,
which `on_quotes` takes in `stocks` (the engine sends it for every underlying whose
equity options or shares the account holds). The price is fresh within
`max_quote_age` of the market time while the stock market is open, or of its last
close (16:00 ET, 13:00 early; `md::stock_session`) while it is not, so the close
stays current while the options trade on to 16:15 and overnight. After a business
day's regular close, Cboe supplies its positive finite `data.close` as the underlying
price, including revisions, so delivered shares use that close rather than
after-hours trades; without a valid close it keeps the current quote
([Cboe clocks](runtime.md#product-sessions-and-cboe-clocks)).
They count in equity, daily loss and the rules, at their dollar
delta in risk limits and scenarios, and in the P&L by Greek (all delta). Short shares
hold 150% of their value in buying power, and under strategy margin every 100 shares
can cover an option (see buying power under Account rules). `trade_stock(symbol, shares, time)` only
reduces them, at the underlying's fresh price in the regular session and without a
fee; flattening closes them too while the stock market is open (after its close they
stay, and the flatten lists them in `kept_stocks`), and a decided attempt liquidates them, and a reset
drops them at their mark. With the `buying_power` rule, a sale of shares that a short
call is written against, which leaves the call naked, must leave available buying power
nonnegative when it reduces free buying power, otherwise `BUYING_POWER`; buy the call
back first. Every change is a `StockFill` in `TradingSnapshot::stock_fills`,
with how it came about (`StockSource`: delivery at settlement, early exercise, a trade,
the rule, a reset) and the delivering option, so the trade history follows the shares
too.
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
of market/IOC or limit/DAY/GTC/IOC. Market/DAY or market/GTC, market with a limit, and limit without
a positive price reject. A client ID is 1 to 128 bytes of text without control
characters, and cannot be reused, even after a rejected order.
Submitting the same terms again under a used client ID is a retry, not a new order: it
gets the first answer (the order as it now stands, even after a change, or the original
rejection) and records nothing. Other terms under that ID reject with `DUPLICATE_CLIENT_ID`.

Tickets show liquidity warnings for thin or absent two-sided quotes, including
spread percentage, session volume and OI, without blocking submission. Strategy
tickets check each leg. Market tickets show each executable side's displayed size
and requested contracts. The chain marks a quote paper orders cannot fill on (one-sided,
crossed or without displayed size) and, on the account's quotes, the displayed size
its orders have already taken; tickets show both, and say from the server preview how
much of an order can fill on the current quote and what happens to the rest. Optional latency and impact can change the simulated fill,
as described below.
The [liquidity rules](runtime.md#chain-volume-and-liquidity) are browser cues, not
new reducer checks or journal fields.

Buy execution uses ask; sell execution uses bid. With `slippage_ticks` (0 by default,
an integer from 0 to 10), buys add that many ticks to the ask and sells subtract them
from the bid, floored at zero. The tick is `tick_size(root, displayed_price)`, using
the displayed far side's tier even if slippage crosses $3. A limit executes only if
the displayed far side is no worse than its limit, including equality: buys fill at
`min(limit, ask + slippage)` and sells at `max(limit, max(0, bid - slippage))`.
Bracket exits, liquidation and expiry auto-close use the same slippage. Exercise,
settlement and share trades keep their existing prices. Without impact, market orders
never sweep undisplayed depth.
Unfilled DAY and GTC limits rest; unfilled IOC quantity cancels with `IOC_REMAINDER`.
`filled_quantity` remains separate from terminal state: cancelled orders may have
fills. Each partial fill charges `quantity * fee_per_contract` (default $0.65).

Both positive prices and both positive integer sizes are required; crossed,
one-sided, missing and zero-size books supply **no liquidity**, except to a combo
exit closing a leg that shows only an ask (see Multi-leg orders), and to a buy that
only closes a short, which can take a fresh book that shows only an ask, as a far
option nobody bids for does: a short quoted 0.00/0.05 is bought back at 0.05 within
the displayed size, its price band measured from the mark halfway to the ask. Buying
more than the short, or selling, still needs a two-sided quote. Locked positive
books are accepted. Displayed size is an independent bid/ask budget per
(contract, observation), consumed across all paper orders. The same observation
never refills that budget, including after recovery. Each new observation refreshes
it once. The same observation offered again with a later time confirms the quote is
still current: its time and its mark's advance, and its budget does not.
Fractional provider sizes must be converted conservatively by the caller.

Every fill keeps the book it traded against (`Fill::quote`): both sides and their
displayed sizes, what was left of the taken side's displayed size for paper orders
before it (below zero once impact depth is in use), and when the quote was first
given. `quote_time` is when the quote was last confirmed current. A confirmation of
the same observation at a later time keeps the first time, and so does a quote offered
later than the provider gave it (`QuoteObservation::quoted`: the engine supplies the
provider's own quote time, which a snapshot feed can vouch for long after). A fill on
a quote that stayed unchanged for minutes, or one that latency held back, therefore
shows how old its quote was. The book is written only with fills that have it, so
fills recorded by earlier builds recover without one.

Resting buy limits cross when ask <= limit; resting sell limits cross when bid >=
limit, on a new valid observation timestamped at/after acceptance. A newly
submitted order may execute against the latest cached quote if it is still fresh.
An observation offered again in a later batch refills nothing, but an order that a
data gap held back when it was new (see below) can take what is left of its budget
while it is still fresh.
For each OSI and side, better limit price wins,
then acceptance sequence (order ID). Buy and sell budgets are separate; buys are
processed first for deterministic cross-side risk effects. OSIs are processed in
lexical order after atomic installation of the entire batch. Submissions also
respect existing better orders when sharing the current budget. There is no queue
position, trade-through or hidden-liquidity simulation in v1.

### Optional fill models

**As displayed** is the default: no latency, no impact and no slippage. Existing
plans, account responses and journal records keep their previous bytes when the new
settings are zero. The Rules page offers **Conservative** when starting a plan for
an account's new attempt: 1,000 ms latency, 1 slippage tick and 1 impact tick. This
adds friction for practice on a delayed feed; it does not reconstruct a live market.
The choice does not change the plan's evaluation or margin rules. Custom account
rules can set each value separately. Changing a preset starts a new attempt, with
the usual reset of positions and cash; it does not change a running attempt.

`impact_ticks` is an integer from 0 to 10. Zero keeps the displayed-size cap above.
A positive value supplies **simulated depth** in blocks as large as the displayed
size on that side. Block zero fills at the displayed price with the configured
adverse slippage. Block one adds `impact_ticks` adverse ticks, block two twice that, and so
on. The tick tier stays that of the displayed price; sell prices stop at zero.
For example, with an ask of $4.20, size 2, one impact tick and no slippage, a buy of
5 SPXW contracts fills 2 at $4.20, 2 at $4.30 and 1 at $4.40. With one slippage tick,
those prices are $4.30, $4.40 and $4.50.

Depth consumed by earlier orders on that side counts toward the same blocks.
Bid and ask remain independent. A new observation resets the budget; reconfirming
the same observation does not. There is no inferred depth when either displayed
size or price is missing, nonpositive or invalid. With impact enabled, the whole
price including slippage must fit a single-leg limit; slippage is not clipped to
make a deeper block fill. DAY/GTC remainders rest, and IOC remainders cancel.
Normal price-band, loss, buying-power and exposure checks still apply at each fill.

Combos take one whole unit at a time with impact enabled. Each leg consumes its
own side's blocks; a ratio spanning blocks records separate prices, using exact
fixed-point money. All legs of that unit pass checks and fill together only if its
entire net fits the limit. Fees remain per contract. Brackets, manual closes and
account-owned liquidation/auto-close orders use the same impact model.

`fill_latency_ms` is an integer from 0 to 60,000. A positive value holds an accepted
order until every leg has a fresh, valid quote timestamped at or after acceptance
plus that delay. Advancing transaction time alone cannot release it. A later
snapshot of the leg's underlying confirming an unchanged quote can qualify without
replenishing its size; another underlying's snapshot cannot.
On a sparse or 15-minute-delayed feed, the next quote can arrive much later than
the configured delay. There is no wall clock or random timer in this rule.

Stops start their delay when triggered, even if they were armed long before.
Bracket targets start when created by the entry's first fill. In-place modifications
keep the original acceptance time and delay. A delayed IOC stays working until its
first eligible quote, attempts execution once, then cancels any remainder; a limit
can continue waiting for its price. DAY ends, earliest-leg expiry and auto-close
cutoffs run before matching. GTC and bracket exits wait for the regular session.
Cancels and the kill switch still act immediately on pending orders; reduce-only
closes remain eligible. Account-owned closes also wait, without creating duplicate
pending closes. Normal session, halt and risk checks still apply.

Both models are simulations. Neither knows queue position, hidden liquidity, or
whether the market would have traded at all. Impact invents a price schedule, not
observed market depth; latency selects a later supplied quote, not a future trade.
Neither removes the hindsight advantage of a delayed feed.

### Sessions

Orders trade in the sessions `md::trading_session(root, time)` gives each product.
Every product has its **regular** session: 09:30 to 16:15 ET for index roots and the
ETFs whose options trade until then (SPY, QQQ, IWM, DIA, GLD, TLT, the sector SPDRs and
others, `md::is_late_close_underlying`), 16:00 for other equity options, or 13:15 and
13:00 on early-close days. On expiry day, expiring index series stop at 16:00 while
expiring ETF options trade on to 16:15. SPX/SPXW, XSP, VIX/VIXW and RUT/RUTW options also trade in Cboe's **overnight**
(global trading hours) session, 20:15 to 09:25 ET, which belongs to the next trading
date, and in the **curb** session, 16:15 to 17:00 ET after a full day (see
[runtime notes](runtime.md#product-sessions-and-cboe-clocks)). As on Cboe, the overnight and curb
sessions take limit orders only; openport also leaves triggers and brackets out of
them, so they match plain limit orders, single or multi-leg, DAY or IOC. GTC limits,
including those with a trigger or a bracket, and a held spread's exits (`exits_only`)
are accepted there, and between sessions, when the data and other checks permit, but
they wait for the regular session: nothing matches, triggers or creates exits for them
before it. Other market orders, triggers and brackets
reject with `LIMIT_ONLY`, and other orders between sessions with `SESSION_CLOSED`. A DAY
order lasts the session it was accepted in: an overnight order ends at 09:25 with
`DAY_END` and does not carry into the regular session. It never outlasts its earliest
leg's last trade or the account's auto-close deadline either, and its `day_end`
says when it ends: 16:00 for an SPXW series on its expiry day, whose session runs to
16:15. Bracket exits, triggered
orders, GTC limits and the account's own closing orders (liquidation, expiry close) act in the
regular session only, and flattening sends market orders, so outside it the flatten is
refused (`LIMIT_ONLY` or `SESSION_CLOSED`) and leaves the positions and their orders alone.
AM-settled series stop trading at the regular close of the business day before their
expiry, so no curb or overnight session trades them then (`SESSION_CLOSED`); PM
series trade the overnight session of their expiry date. Contract expiry can be
earlier than a session end and takes precedence (e.g. PM expiry 16:00). Boundaries
process on the first command at/after them, before possible fills.

GTC limits survive session ends, daily rollover and journal recovery. They wait
outside the regular session, even on products with overnight trading. They reserve
risk and buying power on every day they rest, just like working DAY limits. Checks
run again before a fill and on limit changes: a failed check cancels with
`RISK_CHANGED`; missing or stale data waits. They cancel with `EXPIRED` at the
earliest leg's last trade, or with `EXPIRY_CUTOFF` at the account's earlier auto-close
deadline, even when no position is held; their `day_end` is that deadline. Evaluation
decisions and account resets cancel them too.
A plain GTC limit can be submitted outside a session if the data and other checks
permit it; submission does not make it match there. Holidays and
early closes follow NYSE's rules from 2022 on, and the overnight session runs into
most holidays until 11:30 ET, as Cboe schedules it (see the runtime notes).

The engine also compares each underlying's market-data time with wall-clock time. A
healthy feed shows the market as it was the provider's stated delay ago, stopping at
the end of a session. When that moment is in one of the product's sessions, or data
stopped inside a session that should since have ended, a lag **greater than
`max_quote_age`** beyond it (for a delayed feed, the larger of that and three
minutes, since delayed snapshots can trail their stated delay by a minute or two)
rejects new orders with `FEED_STALLED`, including the lag
behind the wall clock in the message (for example, "SPX quotes are 10h 20m behind the
market; the feed appears to have stalled"). A feed that rightly shows a closed market
returns `SESSION_CLOSED`, naming why (between sessions, a weekend or a holiday): for
the first 15 minutes of a session, a healthy 15-minute delayed feed still shows the
market before it. The simulation clock stays at market time. Resting orders do not
fill from stale data and are not cancelled merely because the feed stalls; they
remain subject to normal market-time DAY/expiry, risk and explicit cancellation rules.

Snapshot providers (Cboe, Massive, ThetaData and the demo market) send only the
quotes that changed, and end each poll of an underlying with `md::SnapshotComplete`.
While an underlying's last complete snapshot passes that stall check, the engine
offers every account its quotes, valuations and share price as current: valuations
and the share price at the market time, option quotes as described next. So a
quote that sits unchanged, such as a quiet far wing's, stays current, and so does an
underlying whose source runs a minute or two behind the others', as Cboe's quote pages
do. Past the tolerance they are no longer offered, age, and new orders reject; after
a gap, such as overnight, the account waits for the next complete snapshot.
Streaming feeds mark no snapshots, so their quotes keep the time they last changed.

Current is not new. An option quote carries the market time its own underlying's
latest snapshot arrived (or its own later timestamp), not a newer time another
underlying's data has moved the clock to. So fill latency waits for that
underlying's next snapshot: a QQQ poll cannot release an SPX order on the SPX quote it
was sent against. Only once that time falls `max_quote_age` behind the market time
does the quote follow the clock, which keeps it current until its own feed stalls.
Fills record the quote time they used. Replays, backtests and the demo market apply
every underlying's snapshot of one market time together (see
[reproducible runs](architecture.md#reproducible-runs)), so triggers, fills, plan
rules, playbooks and equity samples never see an instant half applied. A live polling
feed's underlyings arrive apart and each is applied as it comes, so there a rule
decided between two polls, such as the expiry cutoff or an intraday floor, values the
underlying still waiting at its last snapshot.

**Circuit breakers** halt the whole market as the exchanges' market-wide rule does
(NYSE Rule 7.12, which the options exchanges follow), measured on the S&P 500 (SPX, or
SPY when SPX is not subscribed) against its previous close: Cboe's published
previous-day close (`md::UnderlyingClose`), a generated scenario's previous close, or
the closing print the engine saw. In the
regular session a fall of 7% (level 1) or 13% (level 2) halts trading for 15 minutes,
each once a day and only until 35 minutes before the close (15:25 ET, 12:25 on an
early-close day); a fall of 20% (level 3) halts it for the rest of the day. While a halt
covers an underlying's market time, new orders reject with `MARKET_HALTED`, whose
message gives the fall and when trading resumes, status reports it, and no resting
order, trigger or bracket exit fills. The terminal shows the fall, the level and the
resume time in a banner while the latest market time is inside a recorded halt;
level 3 says trading is halted for the rest of the day. SPY is named when it stands
in for the index. `server::circuit_breaker` holds the rule. Halts of a single stock or
ETF are not modelled.

With a paper journal, the engine keeps the day's breaker state (the level tripped,
the previous close and the halts) in `market-halts.json` beside the main journal,
replaced atomically on each change, so a restart during a halt keeps its original end
and cannot trip the same level again that day. A new trading day resets the level and
looks up its own previous close; the last day's halts stay listed until another day
trips one, and prints from an earlier trading day cannot reset the breaker. Replays
and engines without a journal keep the state in memory. A missing file starts empty;
an unreadable or invalid file, or a failed write, is reported in status and the
daemon log, and the engine carries on without it.

## Conditional and bracket orders

A single-leg order, or a closing combo, may carry a **trigger** `{source, direction, level}`. It is accepted with the
normal pre-trade checks, then rests as `Armed`: open, cancellable, reserving exposure and
buying power, but never matched. Option triggers compare the order's executable side
(ask for buys, bid for sells) from a fresh book; underlying triggers compare spot from
the contract's fresh valuation. Levels are inclusive (`at_or_below`, `at_or_above`), and
missing or stale data never triggers. Triggers are checked after each market batch's
resting orders match, and at submission, only during the contract's regular session.
A reached order is activated: entries rerun every pre-trade check, while bracket exits
need only an executable book. Stale data or a closed session keeps it armed; any other
failure cancels it with `RISK_CHANGED`. It then trades like any order (market orders
IOC). Armed orders last until the nearest contract’s last trade or auto-close, which
their `day_end` reports. Once triggered, a DAY order lasts the session it activated in,
like any DAY order; a GTC order stays good until that deadline.

A limit order with a trigger is a **stop-limit**. Its limit is set against its stop, not
against today's market, so when it is accepted or changed the price band is measured
around an option (or combo) trigger's level instead of the mid: a protective sell at
2.90 with its stop at bid 3.00 is accepted while the market is at 4.10, and one at 1.00
is not. An underlying trigger's limit is still banded around the mid. Once triggered,
the band applies only to the price it fills at, so a market that gaps through the
limit leaves the order working at its limit rather than cancelling it: it never fills
worse.

A **bracket** `{stop_loss, take_profit}` on an entry creates exits as the entry fills.
Each exit takes a trigger (a stop: market IOC when reached), a limit price (a resting
take-profit), or both (a stop-limit: armed until its trigger is reached, then a GTC
limit at its price, which a gap through it leaves waiting rather than filled at a worse
price). Exits take the opposite side and are sized to the entry's
filled quantity; later entry fills grow them. Client IDs are the entry's with `:stop` or
`:target`. A stop is market IOC, and a take-profit or a triggered stop-limit a GTC limit
on the tier tick; all last, and report
in `day_end`, until the nearest contract's last trade or auto-close, and a stop already
reached when the entry fills fires at once. The two exits are linked: when one fills
completely it cancels the other with `OCO_FILLED`, and a partial fill of one leaves the
other in place, shrunk to what is still held. A stop that fills only in part, because the
bid shows fewer contracts than it sells or other orders have used them, does not cancel
its remainder: it re-arms for it (`order_rearmed`) and fires again on the next quote that
still reaches its level, so a thin book never leaves part of the position bare. If the
entry still has an unfilled remainder, the first exit fill cancels it with `OCO_FILLED`
so it cannot reopen after protection has fired. Exits never exceed the position they protect: they shrink when it shrinks
and are cancelled with `POSITION_CLOSED` once it is flat, so they never open a
position. Because they only reduce risk, they execute like system orders, without the
price band or loss projection, and good-until-expiry exits wait for the next regular
session. A bracket pair counts once in reachable exposure and buying power (fees only);
exits never count against buy-only sells, so a manual close is always possible.

## Multi-leg orders

An order with **legs** trades two to four contracts together: each leg names a
registered contract, a side and a ratio from 1 to 10, all on one underlying (expiries may
differ, so calendars and diagonals are allowed). A roll may take up to eight legs, so
an iron condor rolls whole in one order: past four legs, every leg but four must close
held contracts in full for its units, or the order is refused (`INVALID_ORDER`, with
the opening legs counted against four). The order has no symbol or side;
its `quantity` counts units, and `limit_price` is the net per unit, positive
for a debit paid at most and negative for a credit received at least (zero is even).
Market orders are IOC as usual. The net must be a multiple of the smallest lower-tier
tick among the legs ($0.05 for SPX-class roots, $0.01 for XSP and equities).

Checks run per leg where they apply: registration, expiry, the session, a fresh
executable book, and `units * ratio` within `max_order_contracts`. The net price must lie
in the price band around the net mid (a closing stop-limit's, around its combo trigger
level), where the band is as wide as the one for the legs'
gross premium (`max(absolute, relative * sum of ratio * mid)`). Buy-only plans reject
opening multi-leg orders (`BUY_ONLY`); inside the pre-expiry cutoff only closing
orders are accepted (`EXPIRY_CUTOFF`), and daily loss, exposure and buying power apply to the whole order.

A multi-leg order fills **all legs together**, in ratio, when the net at the slipped
far sides (asks plus slippage for bought legs, bids less slippage for sold legs) is
at or below its limit. Every leg takes its full slippage; an order whose net would
exceed the limit waits, rather than allocating a partial slip among its legs.
The net may improve on the limit. Without impact, units are bounded by every
leg's remaining displayed size. With impact, each leg uses the simulated blocks above. Multi-leg orders match after single-leg orders on the
same books, in acceptance order, and only on quotes newer than their acceptance (except
at submission). Each leg's fill is recorded under the order's ID; `filled_notional` and
the average fill are the net per unit. The projected fill is checked for daily loss and,
when any leg opens contracts, buying power, exactly like a single-leg fill. A working
multi-leg order counts as one pending exposure (its legs summed), and it is cancelled at
its session end or its earliest leg's last trade or auto-close deadline, whichever comes
first, like any DAY order. GTC limits instead last until that deadline.

A multi-leg **entry bracket** reverses every entry leg, keeping its ratios. Entry
brackets require opening legs; a roll or close cannot create a bracket on reversed
legs it no longer holds. Exits are created on the first fill and grow with partial
fills. Each exit fills all its legs together, and the pair counts once in risk and
buying power. It remains reducing under the kill switch and must keep a defined-risk
plan's shorts covered. Manual closes shrink or cancel the exits.

Take-profit limits use the closing order's signed net: positive for the maximum
buy-back debit after a credit entry, negative for the minimum credit after a debit
entry. The ticket expresses this as a percentage of the entry net: 50% buys back
half a credit, 150% receives one and a half times a debit. Prices snap to the combo
tick; an exit price off it rejects with `INVALID_TICK`, as a single contract's does off
its tier tick. This fixes a price when submitted; it does not reprice with later entry fills.
A target waits until the entire slipped closing net fits its limit.

A stop has `source: "combo"` or `"underlying"`. Combo levels compare the sum of the
closing legs' displayed asks for buys minus bids for sells, weighted by ratio,
without slippage. Negative levels are allowed. A leg whose fresh quote shows only an
ask, as a far option nobody bids for does, counts at that ask when the exit buys it
back, and at zero when the exit sells it. The exit trades it that way too: it buys
at the ask within its displayed size, and gives a long away at 0.00, without a size
limit and with the usual fee. So a worthless wing cannot hold a bracket's or held
spread's stop or target, or a flatten, back; a plain triggered combo, which is not an exit, still
needs a two-sided quote on every leg. Underlying levels must be positive
and read the first leg's fresh valuation. On reaching the inclusive direction, the
stop sends a closing market IOC combo with normal slippage and displayed-size
limits; a stop-limit (a trigger and a signed net `limit_price`) rests instead as a
closing GTC combo limit at that net, on the combo tick, and fills only at it or better. A partial stop fill re-arms the stop for the units still held and shrinks the
target to them, as for a single contract. Missing data never triggers. Plain triggered
combos are accepted only when every leg reduces holdings, including other manual
closing orders' claims. Opening combos cannot carry triggers.

To attach exits to a held spread, submit its **closing** legs with `exits_only: true`
and `bracket`. No entry fill is generated. The submitted order is the take-profit
(or the stop if there is no target); its `type`, `limit_price` or `trigger` must match
that exit. Use GTC for its limit (a stop-limit's included, with its trigger) or IOC for
its triggered market order. Both exits
last until the nearest leg's last trade or auto-close. They can be attached in any
session, and outside the regular session they wait for it. Every leg must oppose a held
position, and `quantity * ratio` must fit the holding. For example:

```json
{
  "client_order_id": "held-put-exits",
  "legs": [
    {"symbol": "SPXW  261022P04900000", "side": "buy", "ratio": 1},
    {"symbol": "SPXW  261022P04890000", "side": "sell", "ratio": 1}
  ],
  "quantity": 1, "type": "limit", "time_in_force": "gtc", "limit_price": "0.40",
  "exits_only": true,
  "bracket": {
    "take_profit": {"limit_price": "0.40"},
    "stop_loss": {"trigger": {"source": "combo", "direction": "at_or_above", "level": "2.00"}}
  }
}
```

The response's `oco`, `stop_loss_order` and `take_profit_order` identify the exits.
Change their levels with `PUT /api/orders/{id}`; cancel both IDs to remove the pair.
The held-strategy row's **Exits…** dialog sets, changes and cancels them. Its Roll
dialog can select the put or call vertical of a condor or iron butterfly, a later
expiry and new strikes. The resulting four-leg order closes that side and opens
the new vertical together; the other side stays held. After a full side roll, the
held view shows the two remaining verticals with their current position cost bases,
not the roll’s combined closing and opening net.

## Changing, cancelling and flattening

A resting order changes in place (`modify`): a DAY or GTC limit order, an armed order or a
bracket exit. It keeps its ID, its fills and its place among equal prices. The new
quantity counts filled contracts too and must exceed them; the limit price applies
to limit orders (a multi-leg order's signed net); the trigger level to armed orders
with a trigger. The changed order takes every pre-trade check a new one would, with
its own reservation released first, and a failure leaves it exactly as it was. A
limit that becomes marketable trades against the cached fresh quote, subject to
any configured fill latency, and an armed order whose new level is reached activates in the regular session. Bracket
exits change their level or take-profit price (a signed net on the combo tick
for spreads, positive on the tier tick for a single contract), and their size: at
most the position they protect, so a target can take part off while the stop keeps
protecting the rest (a smaller exit that fills completely leaves the other working,
shrunk to what is still held). They follow the position down as it shrinks, and a
later fill of their entry grows them back to its filled size. A resting limit
entry changes its time in force between DAY and GTC (`time_in_force`): a DAY order
then ends with the current session, a GTC one at expiry; an armed order keeps its
expiry until it triggers. Bracket exits are good until expiry and keep it. The engine applies a new order's feed gate
(`FEED_STALLED`) before a change, because a change can trade.

Every change asked of an open order stays on it in `Order::changes`, oldest first,
applied or refused: its market time and actor, the terms requested (each left empty
kept), the terms the order had then, and the refusal's decision with its numbers.
A refused change leaves the order's terms as they were. `Order::ended_at` is when an
order stopped working: the fill that completed it, its cancellation (whatever the
reason: `USER_CANCEL`, `DAY_END`, `EXPIRED`, `RISK_CHANGED` and the rest) or its
rejection, zero while it is open. Both are journaled only on orders that have them, so
other orders keep their bytes, and orders from older journals load without them.

The snapshot also says what keeps each open order from filling now
(`TradingSnapshot::waiting`, derived from the account's books, sessions and clocks as
matching reads them, never journaled):

| Code | The order waits for |
| --- | --- |
| `TRIGGER` | Its trigger: the reference (ask, bid, the closing legs' net or the underlying), the level and the value now |
| `REGULAR_SESSION` | The regular session: armed orders, GTC orders, bracket exits and triggered GTC orders fill only then |
| `INVALID_QUOTE`, `STALE_QUOTE` | A two-sided quote with sizes, or a fresh one, on a leg; a one-sided book supplies no liquidity |
| `FILL_LATENCY` | A quote stamped at or after its acceptance (or activation) plus the account's fill latency, with the time |
| `NEWER_QUOTE` | A quote newer than its acceptance: resting orders take only those |
| `LIMIT` | The market: the far side (or a multi-leg order's net at the far sides) is worse than its limit, with both prices |
| `DISPLAYED_SIZE` | A new quote: paper orders used the displayed size at this one |
| `STALE_DATA` | Fresh marks on held positions and the valuations risk needs |

An order with nothing visible holding it back has no entry.

`cancel_all` cancels every open order, armed ones and bracket exits included, or
only one underlying's. `close_positions` flattens the account or one underlying and
works until it is flat. It cancels the open orders in scope but the bracket exits,
then closes each unexpired position in scope with market orders under the account's
fill model:

- **Spreads close together.** Each short and the long that covers it (one of its type
  on the same underlying that expires with it or later, as `defined_risk` counts
  cover) close as one two-leg order: the short is bought back and the long sold in the
  same fill, so no short is ever left naked, with or without fill latency. Per type,
  the latest-expiring shorts take their cover first, from the nearest expiry and then
  the nearest strike, which covers the most shorts. The shorts and longs left over
  close alone.
- **Remainders keep working.** The closes are reduce-only market DAY orders
  (`reduce_only` on the order). What a thin quote or fill latency leaves unfilled
  works on later quotes until it fills, its session ends (`DAY_END`), the trader
  cancels it or the position closes otherwise. Like bracket exits, they shrink with
  the position and are cancelled with `POSITION_CLOSED` once it is flat, so they never
  open one; they keep working under the kill switch and personal guardrails.
- **Large positions split.** A close larger than `max_order_contracts` goes as several
  orders, none larger than the limit.
- **Exits stay until flat.** Bracket exits keep protecting whatever is still open,
  sized to it, and are cancelled with `POSITION_CLOSED` once it is flat. A pending
  close the account placed itself (an auto-close or liquidation) keeps working too.
- **Thin and missing quotes wait.** A stale, missing or invalid quote only makes a
  close wait for a usable one; it is not refused. A short nobody offers below the
  ask (a 0.00/0.05 quote with no bid) is bought back at its ask within the displayed
  size, and a long nobody bids for is given away at 0.00 with the usual fee, as a
  combo exit does, so a worthless wing cannot hold the flatten back.

Being reduce-only, the closes execute like bracket exits and the account's own
closes: without the price band, loss projection, exposure or buying-power checks,
and without needing fresh marks on every other position, so an expired position
awaiting its settlement no longer blocks them. They are the trader's own orders
(client IDs `openport-close-{version}-{n}`, numbered past any client ID the account
has already used). Expired positions wait for settlement, and so does a long that
covers a short that has expired. Delivered shares in scope close at the underlying's
fresh price in the stock market's regular session. The response lists every position
in scope still open (`residuals`): the contracts still held, those its closes are
still working, and why the rest are not being closed (a refusal,
`AWAITING_SETTLEMENT`).

Before it cancels anything, a flatten checks what each close needs whatever the price:
an attempt still open (`EVALUATION_CLOSED`), the underlying's feed
(`FEED_STALLED`, `MARKET_HALTED`), and a session that takes market orders (`LIMIT_ONLY`
in the overnight and curb sessions, `SESSION_CLOSED` between them) or, for shares, the
stock market's regular session and a fresh price (`SESSION_CLOSED`, `STALE_QUOTE`).
An underlying where every close is refused keeps its open orders, exits included,
and its closing orders are recorded as rejected with the reason; shares that cannot
close stay, listed in the response's `kept_stocks`. When nothing in scope can close
(every position in the overnight session, or only shares after the 16:00 stock
close), the flatten itself is refused with the first such reason, its `scope` the
underlying, and changes nothing.

A flatten has a dry run, `POST /api/positions/close/preview`, with the same optional
`underlying`. It runs the flatten on a private copy of the account at the current quotes,
as a preview does: it writes no journal, allocates no IDs and takes no displayed size. It
answers 200 with `decision` (`ok`, or the reason the flatten would be refused, with
`reason`), the open orders it would cancel (`cancelled_orders`), its closing `orders`
without IDs (symbol, underlying, side, quantity, filled quantity, average fill price,
status and reason: `working` for a close that waits for fill latency, `rejected` with the
reason for a refused one), their `fills` now, the shares it would close (`stock_fills`) and
keep (`kept_stocks`), what would still be held in scope (`remaining` contracts and
`remaining_shares`), and the account before and after it (`current` and `after`, as in a
[what-if](#what-if)). A close waiting for a later quote is shown working, so the account
after it still holds that position. Flatten in the terminal shows the dry run before
the flatten is confirmed.

### Disposing of worthless positions

A short nobody bids for still has an ask: an ordinary closing buy takes it (see
[quote matching](#orders-and-quote-matching)). A long nobody bids for cannot be sold,
so `abandon(symbol, time)` gives it up: the whole long leaves the account at 0.00,
without a fee, as an `Abandon` closure, and the trade history closes it there. It
needs a long that cannot be sold now: its fresh quote shows only an ask, or it has
expired and waits for its settlement (`INVALID_ORDER` "Someone bids for it" while a
two-sided quote is fresh; a stale or missing quote is refused with its reason).
Abandoning an expired position that would have settled in the money forfeits that
value. The long's bracket exits and the orders selling it are cancelled with
`POSITION_CLOSED`, since a sell would now open a short. It is allowed under the kill
switch and personal guardrails, may not leave a defined-risk plan's short uncovered
(`DEFINED_RISK`), and with `buying_power` a short it covered must still fit. The
portfolio marks such positions `no_bid`. An expired position awaiting settlement
blocks other orders (`STALE_QUOTE`, "All held positions need fresh marks"), so
abandoning a worthless one lets trading resume before an AM settlement arrives.

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
Position realised/fees describe the current open lifecycle: after a reversal, the new
position has realised nothing and carries the excess's share of the reversing fill's
fee, as the trades view splits it. Beside them, each position in `GET /api/portfolio`
names its round trip in progress (`trade`, the trades view's ID) and gives the
contract's `lifetime` this attempt: how many `round_trips` it has had, counting the
open one, and their `realised` P&L, `fees` and `net` together. Delivered shares do the
same, with dividends in their realised P&L.

```text
market value = q * M * mark
unrealised P&L = market value - signed basis
equity = cash + sum(market value)
equity = initial cash + realised P&L + unrealised P&L - fees
```

Fresh valid two-sided books establish a midpoint mark, rounded to the nearest
micro-dollar (a half micro-dollar rounds up). A quote with an ask and no bid, as a far
option nobody bids for shows (0.00 / 0.05), marks its position halfway to the ask and
keeps it current, though only a two-sided quote trades. An invalid or stale observation
never replaces the last valid mark with zero. Snapshots retain mark time/age, the last-mark
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
point): equity less the day's baseline. The HTTP API gives the parts and the total to
the micro-dollar, as the account's money is, and still adds up exactly: each part is
rounded down or up, the parts that rounding down cuts most taking the micro-dollars it
leaves over (largest remainder). A stretch whose mark moved without valid valuations at
both ends cannot be split, so all of it is `other`; `fallback: true` says some of the
parts came from such a stretch, on the account's, a contract's and a round trip's
parts alike. Rollover records the finished day's parts in
its `EvaluationDay` and starts every stretch again from the closing marks; a reset
starts afresh. `GET /api/portfolio` gives each held position's parts, and lists the
contracts and shares traded today and no longer held under `closed` (`symbol`,
`kind` `option` or `shares`, `underlying` and `attribution`), so the rows still add up
to the account's parts after a close.

Each round trip also keeps its own parts over its whole life, across days: the
stretches it was held for and the costs of its fills, the open stretch to the marks
now included, which add up to its P&L (realised less fees, plus the rest at its mark).
A reversing fill books its closing part to the round trip it ends and the rest to the
one it opens. Trades and share trades carry them as `attribution`; a round trip open
from before an upgrade has none (null). Stretches, finished parts and each round
trip's are state, journaled and recovered; a position held from before an upgrade
joins the day's parts at its next fill or rollover. The parts
are analytic dollars, not accounting: vega is per volatility point and theta per
calendar day, as in the valuations.

## Risk and kill switch

Valuations come from the caller's coherent strike-smile frame: spot delta/gamma,
vega per vol point, theta per calendar day, spot S, forward F, discount D, years T
and **the smile IV actually used for the Greeks**. All must be finite; S/F/D/IV must
be positive and T nonnegative. When a held option's strike has no smile IV, as a far
wing without a bid can leave it, the engine values it at the option's own ask IV (or bid
IV) on the expiry's forward instead. Quote and valuation max age default to 60
market-time seconds, configured independently. Missing or stale portfolio/pending-order values
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

An order is refused only when, with it pending, a bucket's reserved risk exceeds its
limit **and** exceeds that bucket's reserved risk without the order. An order that
cannot raise any bucket's reserved risk is accepted, and fills, even while the held
book is already over a limit, whether the market or a tighter limit put it there: a
close, a hedge no larger than the position, or an order on another underlying that
fits that underlying's and the aggregate's limits. Orders that add to an over-limit
side still reject.

Pre-trade checks require a supported registered unexpired contract, an open session that takes the order,
valid order/tick, max order quantity, valid fresh quote, price band, complete marks
and valuations, daily-loss allowance and exposure reservations. The kill latch and
daily-loss checks allow reducing orders as described below. Price protection is inclusive:

```text
abs(price - midpoint) <= max(absolute_band, relative_band * midpoint)
```

At acceptance, price is the limit or slipped market far side. At fill time it is the
actual slipped price, capped by a single-leg limit, allowing favorable moves without
comparing a stale limit to the new mid.
Checks rerun against current state before each proposed fill. Fill projection also
includes spread and fees in daily loss for orders that can open or increase
exposure. A failed fill check cancels the remaining order with `RISK_CHANGED`,
preserving the underlying reason in its message and actual/limit/scope. A data gap is not a failure: when held positions' marks are stale
(`STALE_QUOTE`) or valuations are missing or stale (`MISSING_VALUATION`), as when a
batch brings an order's quote before the rest of the portfolio's after a stall, the
order keeps working and a later batch fills it once the data is complete.
Noncrossed resting orders wait; invalid quotes supply no fills.
Limit changes increment a revision and recheck working orders by the same rule, so a
working close survives a tighter limit while orders that would add to the excess are
cancelled with `RISK_CHANGED`. During an evaluation
or funded attempt, each tighter field applies immediately; each looser field waits
for the next trading-day rollover. The complete desired limits are journaled as
`pending_limits`. A later edit replaces the pending request but cannot loosen a
field already active today. Order size, exposure, price bands, daily loss and freshness
windows all follow this rule. Practice limits change immediately. Resetting an attempt
applies its pending limits. Limits themselves never liquidate positions.

Daily loss is `max(0, start_of_day_equity - equity)` including marks and fees. A loss
**strictly greater than** the configured allowance trips the latch **before matching**
on that market batch. Manual `trip_kill` does the same. The account becomes
**reduce-only**: orders that can open or increase a position are cancelled and new
ones reject with `KILL_SWITCH`. Closing orders, Flatten, bracket exits, the system's
own closes and share closes keep working. Early exercise rejects with `KILL_SWITCH`
because it delivers shares. Settlement remains permitted.

An order reduces only when every leg opposes its current position and its remaining
contracts, plus the other working user orders on that side of the same contract,
fit within that position. Multi-leg ratios count in contracts. Bracket exits do not
reserve this capacity: they shrink after each fill and cancel when the position is
closed. Every fill rechecks the holding, so working closes cannot flip it.
Reducing user orders skip the daily-loss allowance and fill projection; quote,
session, price, coverage and other risk checks still apply. Passed or failed
attempts retain their existing restrictions.

`reset_kill` requires a nonblank reason, records the reset, and immediately re-trips
if the loss still breaches. The account remains reduce-only while latched. A new
trading day's baseline permits a reset; rollover alone does not clear a manual or
daily-loss latch.
A reset cannot make stale data tradable.

`roll_day` is an explicit command on a later trading date. It closes the finished day
on the marks the account last published, which must be complete: the engine rolls an
account over on a new trading date's first market batch, before that batch's quotes
replace them, so the overnight move and fills at the open belong to the new day. A
trading date (`md::trading_date`) is a business day's New York date until
17:00 ET, when its last session (curb) ends; after that, and over weekends and
holidays, it is the next business day, whose overnight session opens that evening. So
an overnight trade counts toward the day it trades for, and a day's close is the last
marked equity before 17:00. Rollover first monitors the old daily baseline, then
stores the new baseline. Repeated same-day rollover rejects. Manual and daily-loss kill latches survive rollover and recovery. Personal daily
latches clear at rollover; cooldowns expire on market time.
There are no deposits/withdrawals or cash interest. Without
the `buying_power` rule, negative cash and short positions are permitted subject to
the stated limits. With it, the selected margin requirement below applies; neither is a
full brokerage margin model.

### Personal guardrails

Guardrails belong to the account, separately from its plan, and default to off.
`PUT /api/risk/guardrails` takes `expected_revision` and a complete `guardrails` object:

| Field | Meaning; zero disables |
| --- | --- |
| `soft_floor` | Decimal dollar equity level above the plan floor |
| `soft_floor_percent` | Whole percent, 0–100, of the plan's drawdown distance to keep above its current floor; the higher of this level and `soft_floor` wins |
| `max_opening_trades` | Number of opening executions per trading day; a partial execution counts once and an atomic multi-leg execution counts once |
| `cooldown_minutes` | Market minutes without new opening orders after a stop-loss exit, up to 1440 |
| `cooldown_loss` | A closing fill's realised loss before fees must exceed this dollar amount to also start the configured cooldown |
| `profit_lock` | Day's marked P&L at or above this dollar amount makes the account reduce-only |

Touching the soft floor submits closing orders and latches `SOFT_FLOOR`. Liquidation
uses the same quotes, displayed liquidity and regular-session restrictions as a plan
floor breach, and retries remaining positions on later updates. It does not itself
fail the attempt. `TRADE_LIMIT` and `PROFIT_LOCK` leave positions open and cancel
opening orders. Working opening orders cancelled by a guardrail carry its code and
message, as new ones refused by it do, not `KILL_SWITCH`. All three last until
rollover. `COOLDOWN` lasts until the journaled `cooldown_until`, including across rollover; wall time does not shorten it. A later stop restarts it, and a longer
cooldown setting extends one already active. Closing orders, Flatten and exits keep
working under all four reasons. Manual reset cannot bypass an active guardrail.

Guardrails are tighten-only within the day on every account, including practice.
Enabling a rule, raising a soft floor, lowering a trade/profit/loss threshold or
lengthening a cooldown applies now. Other changes appear in `pending_guardrails`
until rollover. Starting a new attempt applies pending settings and clears their
progress; personal settings otherwise persist. A soft floor still above equity can
trip again after rollover. The existing kill latch is shared; a manual or daily-loss
trip still requires its own reset after the personal rule expires.

`GET /api/risk` includes active and pending settings, `pending_effective`
(`next_trading_day` or null), the last `pending_applied_at`/`pending_applied_day`, and
`guardrail_state`: opening count, latched reasons, effective soft floor, cooldown end
and market-time seconds left. Rules and Risk show pending values beside active ones.

### Order preview and breach risk

`POST /api/orders/preview` takes the same order body as submission, plus optional
`floor_share` in (0, 1], default 0.5. The const reducer path shares submission checks
and makes a private full-size projection. It writes no journal, changes no account or
ID counter, and consumes no displayed size. The normal HTTP write protections apply.

The response contains `decision` (`ok` or a reason code), `reason`, `buying_power`
(`required`, `before`, `working`, `after`), `exposure_change` (dollar delta, dollar gamma per 1%,
vega and theta), `max_loss`, `max_loss_basis`, `equity_at_max_loss`,
`breaches_floor`, `breaches_soft_floor`, `max_units`, `max_units_buying_power`,
`max_units_floor`, projected `breach`, `execution` and
`liquidity`: for each leg, its `symbol`, `side` and `contracts`, whether paper orders
can fill on its quote now (`executable`, with the `INVALID_QUOTE` or `STALE_QUOTE`
`reason` when not, and a one-sided, crossed or sizeless quote named in its message),
the `displayed` size on the side it takes and `size_left`, what this account's orders
have left of it on that observation. Orders beyond `size_left` wait for a new quote,
or cancel as `IOC_REMAINDER`, unless impact supplies simulated depth. Missing
inputs produce null analytical values. `simulated: true` labels the projection.
Market orders use slipped far sides, through the impact blocks a fill would walk; limit
orders use their limit debit or credit.
Fees are included. Identical client-ID retries return their original decision with no
additional buying power or Greek change; loss projection and sizing are unavailable.

`execution` is what submitting the order now would do, from the same acceptance and
matching run on a private copy of the account: its `status` just after submission
(`filled`, `partially_filled`, `working`, `armed`, `cancelled` or `rejected`),
`filled_quantity` and `remaining_quantity`, the `reason` its rest ended at once (an IOC
remainder past the displayed size, say) or it was refused, its `fills` contract by
contract with their prices, and `average_fill_price`. As displayed, a market order
larger than the displayed size fills that size and cancels the rest; with fill latency
nothing fills on acceptance and the order works until a later quote. `schedule` prices
the full size at the current far sides with the account's slippage, block by block
through impact as a fill would walk them and whatever the displayed size, each block
as `symbol`, `side`, `quantity` and `price`, and `average_price` is its average per unit
(a multi-leg order's net debit, negative for a credit). The full-size projection below
uses those prices for a market order and the limit for a limit order.

`buying_power.required` is what the order reserves while it works. `before` is the
available buying power now, `working` the available buying power while the order works
before any of it fills, and `after` the available buying power once the order has
filled in full at the projected price: not `before` less `required`, since a fill that
releases margin, as a close or a roll does, raises it. An `exits_only` pair rests until
one of its exits fills, so its `after` is the buying power once it is accepted, with its
fee reservation held, as the account shows after submitting it; its loss and Greek
change still project the primary exit (the target, or the stop without one) filling.

For an order that opens every leg, a bounded same-expiry payoff has an exact maximum
loss at zero or a strike; a net short call tail is unbounded. Other orders use the worst
loss of the projected account on its configured spot × volatility grid, labelled
`scenario_grid`: among them every order that reduces a holding, such as a close, a roll
or a sale of more than is held, so selling held longs is never priced as writing new
shorts, and a close's loss is its cost against the marks plus whatever the account still
holds could lose. This finite grid is not a bound on all possible losses. The exact
`expiry_payoff` value describes the order's own payoff, while the scenario value
describes the account after the order. A bracket entry's projected account includes its
exit pair, so `buying_power.after` holds the exits' fee reservation as the account will.
`max_units` fits buying power, pre-trade limits and the requested share of current
room above the nearer plan or soft floor. Touching a floor never fits. The share is
rounded down to millionths for fixed-point sizing. `max_units_buying_power` is the
same size with buying power and the limits alone, floor room aside, and
`max_units_floor` the size whose loss fits the floor share among those the pre-trade
checks accept, without the fit to available buying power (a plan's `buying_power`
rule still refuses); it is null on an account with neither a plan nor a soft floor.
Zero means no size fits: one unit already needs more buying power than is available,
or raises an exposure or order-size limit. Null means sizing is unavailable: an
identical retry, an account stopped by a journal failure, held exits (their size
follows the position), an order the checks refuse at any size for another reason
(a closed session, a stale quote, the kill switch, a price off its tick), or a loss
that cannot be projected without marks and valuations. The preview is a current projection,
not an execution promise; real orders still take all checks when submitted and filled.

`POST /api/orders/{id}/preview` previews a change to a resting order without making it.
It takes the fields `PUT /api/orders/{id}` takes (`quantity`, `limit_price`,
`trigger_level`, at least one of them) and `floor_share`, and answers with the same
response. Its `decision` is the change's, from the checks a change takes with the
order's own reservation released, and an unknown or finished order answers
`UNKNOWN_ORDER` (404) or `ORDER_TERMINAL` (409) as the change would. `execution` is what
the change would do now, on a private copy of the account: a limit repriced through the
market fills at once (`filled_quantity` and `fills` count only what fills now), an armed order whose new level is reached activates, and a refused
change leaves the order as it stands, with its status and remaining quantity. The rest
projects the order on its new terms as a new order is projected: `required` is what it
would reserve, `working` the buying power while it works on them (its current reservation
released), and `after`, the loss and the Greek change are for its remaining units filling
in full, its filled ones already in the account. Sizing counts the units the order could
still work beside those already filled, so the quantity to send is the filled quantity
plus `max_units`; a bracket exit's size follows its position, so its sizing is null.
Edit order in the terminal shows this preview as the terms change.

`GET /api/risk` and `GET /api/account` expose `breach`: dollar `room` and `soft_room`,
`complete`, and `underlyings[]`. Each held underlying has `spot`, `close_sigma` (one
standard deviation of its log price to today's close), `complete`, and optional
`down`/`up` levels with signed `points`, `percent` and `touch_probability`. The levels
are where equity would reach the plan floor, or the personal soft floor on an account
without one (a practice account, say); with neither floor, held underlyings are listed
without levels. `soft_down`/`soft_up` are where equity would reach the soft floor,
which liquidates first when it sits above the plan floor; without a plan floor they
repeat `down`/`up`, and without a soft floor they are null. One underlying moves at a time while volatility and option
life stay fixed. The scenario solver scans by 0.25% to −99.75% and +100%, then by 1%
to +1000%, and bisects the first crossing. A missing level means no crossing in that
scan, not safety outside it or between scan points. Missing valuations leave levels
unavailable.

Touch probability is a model estimate from the driftless log-return reflection formula
`erfc(abs(log(level / spot)) / (sigma * sqrt(2)))`, about twice a terminal tail
probability, where `sigma²` is the market's implied variance to that trading day's
regular close: the nearest expiry's at-the-money IV² times its time to settlement (the
calendar time the IV was solved on), of which today's session takes its share in
regular-session time. A 0DTE expiry gives today's variance directly; a Friday expiry
seen on Wednesday morning gives today about 5.5 of its 18.5 remaining session hours.
Calendar time would spread that variance over nights and weekends and understate the
hours left today several times over. After the close the probability is zero; without
an IV it is null. These are simulated scenarios and
model estimates, not observed market outcomes. Dashboard, Risk and both ticket
previews label them accordingly. Tickets debounce previews, offer **Size to floor**,
and show an explicit failure without guessing when the endpoint is unavailable.

### What-if

`POST /api/orders/what-if` compares candidate adjustments against the held book before
any is sent. It takes `candidates`, one to six, each with an optional `name` (at most 64
bytes) and `orders`: one to four orders as `POST /api/orders` takes them, a client ID
optional. A candidate's orders are checked in turn as submission checks them, each after
the ones before it have filled, and filled in full at the preview's projected prices:
slipped far sides through the impact blocks for market orders, the limit for limit
orders, fees included. Closing the tested side and opening a new spread is one candidate
of two orders. It runs on a private copy of the account, like a preview: it writes no
journal, allocates no IDs, takes no displayed size, and the normal write protections apply.

The response has `current`, the account as it is, and `candidates`, each with its
`name`, its `decision` (`ok`, or the first order's refusal, with `reason`), each order's
`decision` and `reason` in `orders`, and `after`, the account once every order has
filled, which is null when an order cannot be projected (an unknown contract, or no
quote). A refused candidate is still projected, so its risk shows beside the reason it
would be refused. Each account has `equity`, available `buying_power`, the book's
`exposure` (dollar delta, dollar gamma per 1%, vega and theta; null while risk is
incomplete), `max_loss` (the worst cell of the configured spot × volatility grid, from
today's equity, so a candidate's cost to trade counts), `equity_at_max_loss`,
`breaches_floor` and `breaches_soft_floor`, `scenarios` (each cell's P&L from today's
equity, spot-major, in the grid of `GET /api/risk`) and `breach` as above. The grid is
finite and the values are simulated estimates, not execution promises.

Positions (What-if) lists candidates built from a ticket's preview (Add to what-if, into a
new candidate or one picked to take them) or from closing picked positions, compares
them in a table with each one's change in Greeks, and shows P&L by spot move at unchanged
volatility. The list is kept in the browser, per account.

### Risk warnings

`GET /api/risk` and `GET /api/account` list `warnings`: what the held book is close to,
most urgent first. Each has a `code`, a `severity` (`warning` when it costs room, buying
power or a fill soon, `info` when it is worth knowing), a `scope` (the underlying, or
`aggregate` for the account), the contract's `symbol` when it is about one, a `message`
in words, and `actual` and `limit` numbers whose meaning depends on the code:

| Code | When | `actual`, `limit` |
| --- | --- | --- |
| `DELTA_LIMIT`, `VEGA_LIMIT` | A bucket's held exposure is over its limit: orders that add to it are refused, while closes and hedges still go | The absolute exposure and the limit |
| `DELTA_HEADROOM` | An underlying is within max(1%, one standard deviation of its move to today's close) of a move that takes its dollar delta to its own limit or the account's. Per 1% move, dollar delta changes by dollar gamma plus 1% of itself; this is a first-order estimate | The signed percent move, and the threshold |
| `SOFT_FLOOR` | Equity is at or below the soft floor, which closes positions and refuses opening orders until rollover | Equity and the soft floor |
| `SOFT_FLOOR_ROLLOVER` | At rollover the soft floor would be at or above today's equity: pending guardrails apply, and a percent soft floor follows a ratcheted plan floor | Equity and that soft floor |
| `FLOOR_RATCHET` | An active end-of-day drawdown floor that tonight's close at today's equity would raise (and lock, at a lock balance) | The floor tomorrow and today |
| `EXPIRY_DELIVERY` | An American equity or ETF option expiring today a cent or more in the money, without a do-not-exercise instruction: held into expiry it is exercised or assigned and delivers shares, together the strike | Buying power once every option expiring in the money today has delivered at today's price, and zero; `warning` when that is negative |
| `EARLY_ASSIGNMENT` | A short American equity or ETF option the rollover may assign (about half of it, as described under early assignment): marked below its exercise value, or a call with less time value than a dividend going ex within a week and before its expiry; `warning` for tonight's rollover | The mark and the exercise value, or the time value and the dividend |
| `EX_DIVIDEND` | A held underlying (options or shares) goes ex-dividend within a week; `warning` when short shares will pay it | The dividend a share, and null |

Warnings use the account's marks, valuations, limits and the dividend calendar the
rollover pays from. They are views, not rules: nothing is journaled and no order is
placed. Positions (Risk) and Dashboard show them above the breach estimates.

## Playbooks

[Playbooks](playbooks.md) combine versioned setups, server template selection,
preview sizing, GTC entries, brackets and per-account entry limits. Definitions
live beside the main journal, outside reducer state. Staged orders write nothing
until sent through the normal order path. Only replay and scenario accounts allow
auto entries and time stops; live-feed practice accounts do not. Orders retain the
exact playbook version in a tag. The Playbooks page reports adherence and expectancy;
the Journal filters across versions.

`GET /api/account/pass-odds` resamples recorded equity days through the same plan
arithmetic as the reducer. It requires ten completed days with intraday extrema:
with fewer, without an evaluation rule or without complete current marks it returns
422 `PASS_ODDS_UNAVAILABLE` with the reason.
It is an estimate from past results, not a prediction. Per-playbook sampling excludes
days whose account equity cannot be attributed exclusively to that setup.

## Account rules and evaluations

`SessionConfig::rules` (`AccountRules`) turns the account into an evaluation. The
defaults describe the plain paper account above: no target, no drawdown floor, any
side, no buying-power check. All rule money is exact.

| Rule | Effect |
| --- | --- |
| `profit_target` | Pass when equity reaches starting balance + target (zero disables) |
| `max_drawdown` | Fail when equity touches peak − drawdown (zero disables) |
| `drawdown_mode` | `Intraday`: the peak follows every fully marked equity high. `EndOfDay`: the peak moves only at rollover, from the last fully marked equity observed on the finished date. The peak is the high-water mark the floor follows; an account without a target or drawdown (practice) keeps it the same way, although no rule reads it |
| `buy_only` | A sell must close contracts already held, counting working sells on the same contract; otherwise `BUY_ONLY` |
| `defined_risk` | Each short option needs a long of the same type on the same underlying that expires with it or later, any strike (`naked_shorts` counts the rest). An order, single or multi-leg, that would leave more shorts uncovered than before rejects with `DEFINED_RISK`, so closing a short is always allowed. Open orders count as if every sell they offer filled and no buy did (a multi-leg order fills whole; a bracket's two exits sell its position once), so a working sell can never take the long a short needs. Bracket exits and exercise keep shorts covered too. Off in every preset; custom rules take it |
| `buying_power` | New orders and their fills must not take buying power below zero; otherwise `BUYING_POWER` |
| `slippage_ticks` | Integer from 0 to 10 adverse ticks per option fill, including each combo leg and closing orders; default 0 |
| `fill_latency_ms` | Integer from 0 to 60,000 milliseconds on market time before a quote can execute an order; default 0 |
| `impact_ticks` | Integer from 0 to 10 extra adverse ticks per additional displayed-size block; 0 keeps the displayed-size cap |
| `margin` | `strategy` (default) or `portfolio`, selecting the position requirement below. Plans use strategy margin and As displayed fills by default; custom rules can select portfolio margin |
| `expiry_cutoff` | From the last trade − cutoff until the last trade (`OptionContract::last_trade_time`: 16:00 ET on expiry day for index series such as SPXW, 16:15 for ETF options that trade until then, and the regular close the business day before for AM-settled series), every open order on the contract cancels with `EXPIRY_CUTOFF` (DAY, GTC, armed and bracket exits alike, held or not), positions are closed, and only closing orders are accepted |
| `phase` | `Evaluation` (default) or `Funded`; a funded account has no profit target and pays out under `payouts` |
| `lock_balance` | Caps the trailing floor: the floor is the lesser of peak − drawdown and the lock, and once peak − drawdown reaches the lock the floor stays there and stops trailing (zero disables). A lock at or below the starting floor (starting balance − drawdown) therefore fixes the floor at the lock from the start: a static floor, which below the starting floor gives more room than `max_drawdown` alone would |
| `payouts` | Funded phase: qualifying days, withdrawal share, trader split, minimum and caps (see Funded accounts and payouts) |

Outcomes use **fully marked equity**: every position has a mark, fresh or not. A
position without any mark defers the decision rather than counting as zero. Every
transaction runs the monitor after its command and the daily-loss check, as well as
after each atomic fill and before matching a new quote batch: it records
the day's latest marked equity, ratchets an intraday peak, fails on `equity <= floor`
(the floor is breached by touching it) and otherwise passes on `equity >= target`.
Breaches are checked on every transaction in both modes; the mode only controls when
the floor rises. The decision is sticky for the attempt: open user orders cancel with
`EVALUATION_CLOSED`, new user orders reject with it, and every position is liquidated.

**System orders** perform liquidation and expiry auto-close: market IOC orders with
`system = true` and client IDs `system:drawdown:N` (a failed attempt), `system:target:N`
(a passed one), `system:expiry:N` (the expiry cutoff) or `system:soft_floor:N` (a
personal soft floor while the attempt is still active; once the plan decides the
attempt, its own label wins, even when the soft floor latched in the same update).
They need a registered unexpired contract, the regular session and a fresh
executable book and available closing-side liquidity under the selected fill model.
With latency, the close stays pending until an eligible later quote. They skip the kill
latch, price band, daily-loss, exposure, rule and buying-power checks because they
only reduce risk. Without executable liquidity nothing is recorded; the monitor retries
on later transactions until the account is flat, so system orders never accumulate.

**Buying power** is cash less the positions' margin requirement less working-order
reservations under strategy margin, the default, where long premium is paid in full;
portfolio margin, below, takes it from equity instead.
With the default `margin: "strategy"`, a naked short option holds its buy-back
value (last mark, or its entry credit without one) plus the naked requirement
`100 * max(20% of spot - OTM amount, 10% of spot for calls or of strike for puts)`,
with the strike standing in for a missing spot. `margin_requirement` nets spreads,
shares and straddles. A short pairs with a long of the same type on the same underlying
that expires with it or later, as a vertical: a put long below or a call long above its
short costs the width, one at or beyond it nothing, no pair costs more than naked, and
unpaired shorts are naked. Every 100 shares cover an option too, whatever it expires:
long shares make a short call covered, which holds nothing more (the shares are paid
for and deliver on assignment); short shares make a short put covered, which holds its
buy-back value (the short sale's proceeds buy the shares back on assignment); and a long
call caps 100 short shares at its strike, a protected short, instead of 150% of their
value. Shorts and their covers pair to hold the least in total (a minimum-cost
assignment), so one short never takes the long that another short needed, across
expiries or on equal strikes, and a long call goes to the short shares or to the short
call where it saves more. Pairs of options whose shorts expire together hold at most
their combined worst loss at that expiry, a later long counting at its intrinsic value
then, so an iron condor's two wings are not both held. Short puts and short calls the
pairing leaves naked then pair as Reg T straddles or combinations, of any strikes and
expiries: each pair holds the greater naked requirement (buy-back value included) plus
the other side's buy-back value, the greatest of each type pairing together, and a short
still naked takes a vertical's short of the other type when a straddle saves more than
the vertical did. Positions that expire together may instead need their worst loss at
that expiry, when no net short calls make it unbounded. Each underlying needs the least
of pairing across expiries, with straddles and without, and taking each expiry on its
own (the lesser of its verticals and worst loss, with shares covering nothing). So a
credit spread holds its width, an iron condor its wider wing (a calendar beside it adds
nothing), a long butterfly nothing, a calendar nothing beyond its debit, a diagonal the
strike difference when its long is further out of the money, a covered call nothing
beyond its shares, and a short strangle its greater side plus the other side's value.
Structures combine without holding more than each would alone: a butterfly beside a
calendar holds nothing. Long options and long shares are paid in full, so a protective
put needs nothing beyond its premium; short shares no long call protects hold 150% of
their value. A long that expires before its short does not cover it. (European puts can
trade below intrinsic value before expiry; the pairing ignores that.)

With `margin: "portfolio"`, `portfolio_margin_requirement` sums a separate scan for
each underlying, so gains on one underlying cannot offset losses on another. The
scan uses 11 evenly spaced points including both ends: −8% to +6% for index products
(`md::is_index_underlying`, including SPX and XSP), −15% to +15% for stocks and ETFs.
Each option is repriced with Black-76, scaling its valuation's forward with the price
and keeping its implied volatility, time to expiry and discount unchanged. American
options use the same approximation. P&L is the shocked model price less the unshocked
model price, times the signed quantity and multiplier; shares move linearly with
their underlying. Each underlying holds its largest loss, or $0.375 times the
multiplier for every option contract held, long or short ($37.50 per standard
contract), if that is larger, so even a far out-of-the-money long holds the minimum.

Buying power under portfolio margin is taken from equity, as in a portfolio-margin
account: cash plus the positions at their marks (shares at their price), less the
requirement and the reservations. Long options and shares therefore count as
collateral and the account can borrow against them, so cash may go negative, while a
short's value is owed out of the credit it brought in. The price scan and the minimum
follow Cboe Rule 12.4 and FINRA Rule 4210(g); implied volatility is not shocked, and
there is no broker's house margin on top. The scan reuses the risk snapshot's scenario
repricing but its grid is fixed, independent of `SessionConfig::scenarios`. If fresh valuations or share prices are missing, the
snapshot flags incomplete data and uses strategy margin plus the option minimum
until a complete scan is possible; normal order checks still require fresh data.
The historical `short_requirement` field carries the whole requirement in portfolio
mode, longs and shares included; `requirement` carries the same amount under a name
that fits both modes. Strategy mode pairs shares with options as above.

`TradingSnapshot::margin` and the portfolio's `margin` array show what holds the
requirement, one entry per underlying: `{underlying, requirement, parts, scan}`. Under
strategy margin each part names the positions it takes, as `{symbol, quantity}` legs
(the underlying for shares; a position can be split between parts), with its kind
(`naked`, `vertical`, `covered`, `straddle`, `short_shares`, `protected_shares`,
`worst_loss`, or `long` for premium and shares paid in full) and its requirement; the
parts add up to the underlying's requirement, and the underlyings to `requirement`.
Under portfolio margin `parts` is empty and `scan` gives the worst scan point
(`spot_percent`, `vol_points`), its `loss` and the contract `minimum`; while a scan is
incomplete, the entry shows strategy margin's parts with `scan` null and a requirement
that adds the option minimum. The Positions page shows it as the margin requirement
panel. The breakdown is derived from the positions and is not journaled.

Each working order reserves what filling it now would cost: its fees, plus the change
in the positions' margin requirement, plus the premium it pays less the premium it
receives, and never less than its fees (new shorts are valued at the order's price, or
at their marks for a multi-leg order). In strategy mode, an opening buy reserves its premium, a naked
sell its naked requirement (its credit covers the buy-back value), a sell against a held
long (legging into a spread) the width less its credit, a multi-leg credit spread its
width less its credit, and a closing order only its fees. Single-leg orders see the
positions less the contracts that earlier orders, in acceptance order, already claim to
close, so two sells cannot both claim the same long; otherwise each order is measured
against the held positions alone. Portfolio mode uses the same reservation and
projected-fill checks with the scan in place of strategy margin, and because portfolio
buying power is taken from equity, the new contracts count at their marks: a buy
reserves its fees, the scan's change and what it pays above the contracts' marked value;
a sell, its fees and the scan's change less what it receives above their marked value
(either part is negative when the price is better than the mark). That is what filling
the order now would cost the account's buying power. Market orders reserve
what they would trade at now: the slipped far side, through the impact blocks a fill
would walk when the account uses impact; the displayed far side, slipped, when only
that side is quoted; and a buy with no ask at all, its last mark. Limit orders reserve
at their limits. Fees are unchanged.

An order or a fill that would reduce free buying power (cash less the positions'
requirement) must leave available buying power nonnegative, otherwise `BUYING_POWER`
(`RISK_CHANGED` at a fill). One that frees buying power is always allowed: closing a
position, buying back a short and buying protection work even when buying power is
negative. So does an order, and its fill, that would leave more buying power available
than cancelling it, because it lowers what other working orders reserve: buying back the
long that a working sell was written against re-covers that sell, and fills even when
other working sells leave buying power negative after it. A close frees buying
power unless the longs it sells cover shorts that stay open. Selling the long leg of a
spread alone needs enough buying power to carry the short it uncovers, and so does
closing a whole structure, a long butterfly say, whose longs also cover other shorts
held beside it; buy those shorts back first, or close them together as one multi-leg
order (the terminal's Positions page picks positions and does this with Close together). When legging in, a short sold before its long is naked until the long is bought.
Fills recheck against the projected ledger and cancel the remainder with `RISK_CHANGED`.

`reset_account(initial_cash, rules, reason, time)` starts a new attempt. It cancels
working orders with `ACCOUNT_RESET`, records each open position as a `Reset` closure
at its last mark (average price without one; no fill, no fee), archives an
`AttemptSummary`, restores cash, clears the kill latch and applies the new rules. The
order and fill history is kept; `Evaluation::first_order/first_fill` mark where the
attempt begins. Settlements, early exercises and early assignments are also recorded
as closures.

`Evaluation` carries the attempt number, start time, starting balance, peak, floor,
status and decision, plus one `EvaluationDay` per finished trading date (open and
close equity, peak and floor after that day's ratchet, net realised P&L after fees,
and whether it qualified toward a payout), appended at `roll_day`.

### Equity extremes and history

Each finished `EvaluationDay` keeps `low_equity`, `high_equity`, `low_at` and
`high_at`. Current-day values use the `day_` prefix in `evaluation`. Only fully
marked equity contributes, and ties keep the first time. `closest_floor` is the
smallest equity minus floor observed during the attempt, with `closest_floor_at`.
Older records leave these values null and times absent rather than inventing history.

The engine stores marked equity once per market minute, at every fill, when the
floor changes and when equity first reaches the target or the floor. Atomic spread legs
share their post-execution equity. When one update both marks equity and executes
(a pass or breach liquidating at the bid, an exit on a new high), the mark before the
execution is kept at the same time, so a passed attempt's history reaches the equity
that passed it and a trailing floor's new level shows where it was set. Share
deliveries and closes use their committed transaction mark; when several positions
settle at one time, their deliveries share the first fully marked equity after them.
History is
appended beside each journal as `<journal>.equity.csv`, reloaded on start and compacted
to the current attempt plus at most 90 days and 100,000 samples from older attempts.
The CSV columns are nanosecond timestamp, attempt, equity micros, floor micros, peak
micros, target micros, tomorrow-floor micros, option fill ID and share fill ID
(both zero for a minute mark). The earlier eight-column format defaults share ID to zero.
Absent levels are `null`. Invalid/torn rows are skipped and reported. Storage errors
are exposed as `error` and do not stop trading. Replays without a journal store no
history. The server never fills in a gap from its downtime.

`GET /api/account/equity?from=&to=` returns `{samples, error}`. Optional bounds are
inclusive UTC ISO timestamps; `account=ID` selects an account. Each sample carries
`time`, trading `day`, `attempt`, `equity`, `floor`, `peak`, `target`, `tomorrow_floor`
and `fill` (share IDs start with `s`). The Dashboard keeps its daily chart and adds a day-selectable intraday
chart, with gaps, ratchet markers and the target. End-of-day plans also show the floor
that would apply tomorrow if the day ended at that sample; once the attempt is decided
or the floor is locked, that is the floor itself, since rollover no longer moves it. A
finished replay serves its history read-only at
`GET /api/replay/history/ID/account/equity`.

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
it names, otherwise `PLAN_LOCKED`: that preset's starting balance and every one of its
rules, with only the fill model's execution settings free. Custom rules may set `phase`
freely, but may not take a preset's name unless they are that preset (`INVALID_RULES`),
so an attempt recorded under a preset's name was that preset; `--plan` can start a new
journal on a funded preset directly. Preset parameters are this project's own,
modelled on common prop-firm terms: the evaluation's drawdown and strategy rules, the
floor locking at the starting balance, 8 qualifying days of $100, $150 or $200 (25K,
50K, 100K), up to 50% of profit per payout, 80% to the trader, a minimum of 1% of the
balance and caps of 2%, 3%, 4% and then 6% of the balance for payouts 1, 2, 3 and 4+.

`history.hpp`'s `lifecycles(fills, closures, contracts)` rebuilds round trips from flat
to flat. Each replays its own fills through a fresh `Ledger`, so realised P&L uses the
account's basis allocation and rounding exactly; a reversing fill closes one lifecycle
and opens the next at the same price with its fee split pro rata.
`share_lifecycles(stock_fills)` does the same for shares, per underlying: an
assignment that takes more shares than are held closes the round trip and opens the
opposite one at the same price.

## Trade review

Every option fill carries `context`, captured before it changes the account. It
contains `spot`, `spot_source` (`quote`, `parity`, or null), `iv`, `delta`, `years`
to expiry, `equity`, `floor_room` and available `buying_power`. Buying power is the
account's available buying power just before the fill, as the account showed it: the
filling order is already working then, so its own reservation is deducted (for a market
order accepted and filled at once, that is about the value after the fill). Equity and
floor room are the marked account before the order. The reducer copies
its current valuation; it does not calculate Greeks. Missing or stale analytics
stay null. A fresh stock price can supply spot when analytics cannot. Expiry time
is known from the contract even without analytics. Equity uses the account's last
marks and is null if a holding has no mark. Room above the floor is null without a
drawdown rule. Atomic multi-leg fills share the same pre-execution account values.
Older fills have null context. A trade's `entry_context` is its first opening
fill's; `exit_context` is its last reducing fill's, including a partial close.
Settlement, exercise, assignment, abandonment and reset have no closing fill context.

The reducer journals a `TradeReview` per option round trip and per opening
multi-leg order. It samples total marked P&L: realised gross plus the remaining
position's marked P&L, less all fees. Scaling and partial closes keep the same
round trip until flat. Strategy samples combine the legs at the same instant;
their extrema are not the sum of separate leg extrema. Ties keep the first time.
Samples use market time, at quote batches and executions, and finish on closure.
They resolve only to the marking cadence, about 15 seconds on Cboe's delayed feed;
they cannot recover highs or lows between observations. Missing marks are not zero.

The JSON `review` contains positive dollar `mae` and `mfe`, bounded below by zero.
`worst` and `best` retain the actual signed marked `pnl`, UTC `time` and underlying
`spot` (null when unavailable). A settlement sample's spot is the settlement reference,
not the expired contract's last valuation. Closed trades report `give_back = max(0, mfe - net)`
and `r_multiple = net / planned_risk`; `heat = mae / planned_risk`. Planned risk
excludes fees. A single-contract round trip uses its entry bracket's option-price
stop distance from the entry order's average fill price, times every contract
opened and the multiplier: an add keeps the entry's risk per contract wherever it
fills, even past the stop. Later changes to the exit do not rewrite that plan. A
strategy with an entry bracket stop on its combo price plans the same way: the entry
order's net per unit to the stop level (both signed as order prices, so a unit closed
at the stop loses their sum), for every unit that order opened, times the multiplier.
Without one, a strategy uses its opening debit or credit and the minimum payoff at
all strikes and zero, if every leg settles at the same instant and the call payoff is
bounded below. A calendar or diagonal whose every short is covered contract for
contract by longs of its type that expire with it or later, at a strike at least as
good (a call long at or below its short's strike, a put long at or above), plans its
debit: the longs are worth at least what the shorts owe when they expire.
Risk grows with opening quantities, and is not reduced by partial closes. Risk, heat
and R are null without a positive, measurable planned risk, including underlying-price
stops, unbounded structures, and calendars or diagonals with an uncovered short or a
credit. The option legs of a strategy
carry its `strategy_id` and combined `strategy_review` separately from their own reviews.

Strategy review follows the Journal's grouping by the order that opened each
contract round trip. It covers the legs that order opened as round trips of their
own, at least two of them: a roll's new legs keep a strategy review while the legs it
closes keep theirs. An order that adds to another order's round trip shares those
contracts, so it has no strategy review of its own.
Older round trips without entry context keep null review fields: historical marks
are not reconstructed. Share trades retain their existing P&L and notes. Each round
trip's P&L by Greek is its `attribution` (see [P&L by Greek](#pl-by-greek)).

### Return on buying power

A premium-based return misleads for credit spreads: a $1.00 credit on a $5-wide
spread risks $400, not $100. Each trade also gives `buying_power`, what its entry
needed on its own under strategy margin, and `return_on_buying_power`, its net over
that once closed (null while open or when the need is zero). The need is the
premium its opening fills paid less what they received, plus the requirement of its
shorts at their opening prices, with the entry's underlying price for the naked rule
(the strike without one), every contract it opened counted; fees are left out. A
strategy's legs carry the same for the strategy together, `strategy_buying_power` and
`strategy_return_on_buying_power` (once every leg has closed), and a whole trade its
peak: at each opening, what its round trips open then needed, the most of those. The
playbook report and backtest summaries give `average_return_on_buying_power` over
their closed trades, and the Journal, the Playbooks page and backtests show it. It is
the account's strategy margin at entry; portfolio margin or later price changes do not
rewrite it.

### Whole trades

A trade managed over time is one trade, not a string of round trips. Each round trip
is in a whole trade, named by a round trip's ID and carried as its `group`: its own,
or for a strategy's legs the first leg's. A multi-leg order that closes or adds to
another order's round trip at the instant it opens new ones, a roll or an
adjustment, joins the round trips it opens to that trade; so does any order that
names a trade in `group`, by its ID or one of its open round trips' (an order naming
no trade holding an open round trip on its underlying, or attaching held exits, is
refused with `INVALID_GROUP`). `POST /api/trades/group` joins the trades of two or
more open round trips into one, named by the oldest, so legs entered one by one
become one trade. A closed round trip can name its whole trade for grouping while
that trade still holds an open round trip. `POST /api/trades/ungroup` takes each
listed open round trip out of its trade into one of its own; it still refuses
closed round trips. Both refuse fully closed trades and mixed underlyings
(`INVALID_GROUP`) and unknown round trips (`UNKNOWN_TRADE`), are journaled, and
change nothing else.

A trade with more than one entry has a whole-trade review. A roll's starts as the
trade's review was before it (its first strategy's, or its first round trip's), so
its worst and best include the moves before the roll, then samples the P&L of every
round trip in the trade together, closed ones at their final P&L. A trade the account
grouped or ungrouped starts its review again then (`review_since`). `GET /api/trades`
lists such trades under `groups`, newest first and filtered as the trades are: `id`,
`attempt`, `underlying`, `status`, `opened`, `closed`, `trading_day`, `round_trips`,
`entries` (the orders that opened them), `gross`, `fees`, `net`, `unrealised` while
open, `buying_power` and `return_on_buying_power` on its peak, and `review` with its
R-multiple on the trade's net. `GET /api/portfolio` lists
the positions held by whole trade under `strategies`: each trade's open `legs`
(`symbol`, `quantity` and the leg's round trip, `trade`), its `round_trips` and
`entries`, and its `realised`, `fees`, `unrealised` and `net` over every round trip of
it. The Journal shows whole trades as one row each beside strategies, with a leg
taken out in a click, and can count its headline numbers, calendar and reports by
whole trade instead of by round trip; on the Positions page, picked positions group
into one trade.

Each closed option trade says what closed it, `closed_by`: the closure that ended it
(`settlement`, `exercise`, `assignment`, `abandon`, `reset`), or the order of its last reducing
fill: the trader's own `order`, a bracket exit (`stop_loss`, `take_profit`), a
`flatten`, a playbook's close (`playbook`), or a reducer liquidation (`system`), with
`system_reason` saying why: `target` and `drawdown` when a decided attempt liquidates,
`soft_floor` for the personal guardrail and `expiry` for the auto-close before expiry.
The Journal marks such trades, so after a pass or fail the forced exits stand apart.

The Journal shows context side by side, excursions and risk multiples, notes and
tags, and stored one-minute underlying candles with entry, exit, MAE and MFE
markers. Missing candle history is shown as unavailable. The candles route returns
the latest 5,000 bars, so older trades may have partial or no coverage. Strategy
closing prices are net premiums per unit, signed as order prices are (a debit
positive, a credit negative) and shown as the opening price is ("$0.85 db");
return divides leg net P&L by the absolute net entry premium, or is null at zero
premium. Win rate everywhere, the Journal's tile and reports, the playbook report and
backtests alike, is wins over decided trades: a breakeven round trip is neither a win
nor a loss. List filters select
known or unknown planned risk and positive give-back. The weekday and month reports
group closed trades by the session they closed in, each trade's `trading_day`: the
engine's trading date of its close, a business day's New York date until 17:00, and
after that, over a weekend or on a market holiday the next business day, so a close in
Sunday evening's overnight session counts toward Monday and one on the evening before
Thanksgiving toward the Friday. (Trades from older servers have no `trading_day`; for
them the terminal counts weekdays without holidays.) The calendar keeps the New York
date.

## Trade notes and tags

Order requests accept optional `tags` and `note` with the same validation below.
When a fill opens a round trip, these annotate its opening fill ID in the same
transaction. Each newly opened combo leg receives them, so its Journal strategy
and tag reports use them too. Adds to an existing round trip keep its annotation;
closing fills add nothing. Orders preserve the submitted metadata for inspection.
Tickets accept optional tags and notes from their caller; this tree has no strategy
template catalog yet. Older journal orders default to empty tags and notes.

`annotate(trade, note, tags, time)` records the trader's note and tags on a trade,
named by the fill that opened it (the trades view's `id`); `annotate_shares` does the
same for a share round trip, named by its opening stock fill and kept under `s` and
that ID, so the two never collide. The note is trimmed and at
most 2,000 bytes of UTF-8 text, keeping newlines and tabs (a CRLF or lone CR line break
is kept as a newline, and counts as one byte); up to eight tags of 1 to 32
bytes without commas or control characters are trimmed, lowercased and kept once
each. Text past these limits throws `INVALID_NOTE`, whose message says whether the
length or a control character is at fault; a fill that opens no trade returns
`UNKNOWN_TRADE`. An empty note without tags clears them. Notes are journaled
(`trade_annotated`) like any command, so they recover with the account and stay with
its history across attempts, and they are allowed whatever the account's state or
session. The snapshot's `annotations` maps each trade's ID to its note, tags and the
time it last changed.

### Day notes

`annotate_day(day, plan, review, time)` replaces the account's note for a New York
calendar date. Both fields use trade-note validation: trimmed UTF-8 text, each at
most 2,000 bytes, with newlines and tabs allowed. Empty fields clear the note. A
valid date is required; no trade is required on that day. Notes are reducer commands,
journaled with market time and retained across account resets. The Journal calendar
marks days with notes and opens the day's plan and review when selected.
`GET /api/trades` includes `day_notes`, an object keyed by `YYYY-MM-DD`, with
`plan`, `review` and the last edit's UTC `time`, independently of attempt filters.

### CSV downloads

`GET /api/trades.csv` exports option and share round trips; `GET /api/fills.csv`
exports option fills. Both accept `account`, `from` and `to`; dates are inclusive
New York calendar dates. Fills filter by execution date; closed trades by closing
date and open trades by entry date. Invalid dates or an inverted range return 400.
Trades also accept the JSON route's `status` and `attempt`, with CSV defaulting to
all attempts. The Journal's trade download uses the selected attempt; fills always
include all attempts. Export dates do not filter the page's other panels.

Downloads use `text/csv; charset=utf-8`, attachment filenames, a fixed header even
with no rows, CRLF row endings and RFC 4180 quoting. Commas, quotes and newlines in
notes are preserved. Money retains micro-dollar precision, with two to six decimal places. Every JSON
row field is included, with nested context, review and attribution fields in dotted columns;
arrays, including tags and fill IDs, join with `;`. Missing values are empty cells.
Times remain ISO UTC and each row adds `new_york_date`, account, account version,
current provider and a price-source label. Exports from a replay, running or saved, also
name the run on every row: `run_id`, and `scenario` and `seed` or the `recording` it
replayed (empty for a live account), so rows from several practice runs stay apart once
merged; `GET /api/trades` gives the same as `run`. Fill rows carry their `attempt`, as
trade rows do. Share trades list their stock fills with an `s` prefix (`s3`), as their
own IDs have, so they never read as option fill IDs. Paper P&L is simulated; demo prices are
labelled simulated too. Other exports leave price provenance unrecorded because
the current provider cannot establish the source of historical fills. The separate stock-fill, dividend and day-note collections are not
trade rows in these exports.

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
`awaiting_settlement`: 09:30 ET for AM-settled contracts, and for PM-settled ones their
last trade, 16:00 or 16:15 for ETF options that trade until then (13:00 and 13:15 on
early-close days). No underlying quote is automatically taken as settlement.
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
AM-settled series stop trading at the regular close the business day before expiry
(`last_trade_time`) and wait for an explicit settlement value.
American equity and ETF options held into expiry deliver shares at settlement (see
[instruments](#instruments-and-prices)); the settlement closure keeps the option's
trade at intrinsic value.

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

Live paper accounts write and sync every complete line before publishing the
transaction: with `F_FULLFSYNC` on macOS, whose `fsync` leaves data in the drive's
cache for a power loss to lose, and `fsync` elsewhere. This is `FileJournal`'s
default policy for both creation and resume.

Replay, drill and scenario journals, including kept runs, use batched syncs. Each
append writes its complete line immediately; the first record syncs, then an append
syncs when at least 250 ms of steady-clock time has passed since the last sync.
`flush()` syncs pending records immediately, and destruction attempts a final flush
without throwing. Replay pause, stop, finish and teardown flush on the owner thread
before handing on the journal. A process crash loses no completed writes. A power
cut or kernel panic may lose the last quarter second of replay records. Readers can
see complete lines before the sync. Batching is append-driven: an idle journal
waits for another append or a boundary. Compaction and repair still sync fully.

A transaction that would leave less than 64 MiB free on the disk is refused, so a
full disk stops trading without tearing the journal. If a write is torn anyway, resume
refuses the journal until, with openportd stopped, `openportd --repair-journals` cuts
the torn last line off it and each account journal beside it, keeping the original as
`FILE.torn-YYYYMMDDTHHMMSSZ`; damage before the last line is reported and left alone.

An account exists from its journal's first record, its `session_start`. A new journal
whose first record the disk refused (or that a crash cut off before it) is empty, or
holds only that torn line, which the repair cuts off: it holds no transaction, so
nothing is lost by starting over. After freeing space, the main account starts
afresh on its empty journal; startup removes a named account's empty journal and its
name file, so the name can be created again. A failed `POST /api/accounts` removes
the journal and name file it made, and a replay that fails to start removes its
run's files, so neither leaves an account or history entry behind. The repair
reports an empty journal as `empty, holds no transaction`. Opening a journal also
checks, once it holds the lock, that the path still names the file it locked, so a
writer never appends to one another process removed.
An append failure throws `JOURNAL_IO`, leaves the prior account/orders visible, sets the
snapshot's `journal_failed` flag, and refuses every subsequent command. The disk
outcome can be indeterminate after a failed write/sync: **stop trading and recover**;
do not retry against the old in-memory account. A custom `Journal` must honor the
same complete-write or error contract and its chosen durability policy. A deferred
sync failure also latches `JOURNAL_IO`; the replay account stops trading and reports
the failure while preserving its last published state. File creation uses restrictive
permissions; the hosting application should durably provision the containing directory.

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
| `INVALID_ORDER`, `DUPLICATE_CLIENT_ID`, `INVALID_TICK` | Malformed order, a key reused with other terms, invalid price increment |
| `INVALID_QUOTE`, `STALE_QUOTE`, `MISSING_VALUATION` | No executable book, stale/incomplete marks, missing/stale/invalid Greeks |
| `MAX_ORDER_CONTRACTS`, `PRICE_BAND` | Quantity or protected-price bound exceeded |
| `DELTA_LIMIT`, `VEGA_LIMIT` | The order raises worst reachable exposure above an underlying/aggregate limit |
| `SOFT_FLOOR`, `TRADE_LIMIT`, `COOLDOWN`, `PROFIT_LOCK` | Personal guardrail is active; opening orders and manual latch resets are refused while closing orders and exits remain available |
| `DAILY_LOSS`, `KILL_SWITCH` | Daily equity allowance breached, or an order would open/increase exposure (or exercise) while the kill latch is active |
| `RISK_CHANGED` | Fill/limit-change recheck failed; original cause at the start of the message, its `actual`, `limit` and `scope` kept on the order |
| `IOC_REMAINDER`, `USER_CANCEL`, `DAY_END` | IOC remainder (a stop exit's re-arms instead), explicit cancellation, the end of a DAY order's session (a triggered one's activation session) |
| `SESSION_CLOSED`, `EXPIRED`, `AWAITING_SETTLEMENT` | Outside the product's sessions (or an AM-settled series after its last regular close), expiry or last-trade boundary, or pending settlement quality flag |
| `LIMIT_ONLY` | The overnight and curb sessions take plain limit orders: no market orders, and triggers or brackets only on GTC limits (and a held spread's exits), which wait for the regular session |
| `FEED_STALLED` | Market data lags what a healthy feed would show by more than `max_quote_age` (a delayed feed: at least three minutes); message includes the lag behind the wall clock |
| `REPLAY_FAST_FORWARD` | The replay is preparing its start state; wait before submitting orders or changing playback |
| `REPLAY_STEPPING` | A lockstep step (`PUT /api/replay {"until"}`) is playing; orders wait for its response and then use the paused market time |
| `REPLAY_READ_ONLY`, `REPLAY_RUNNING` | A finished run refuses writes; a running run cannot be opened as history or deleted |
| `RUN_ENDED` | A saved replay run's order that was still working or armed when the run ended: its archive shows it cancelled, as it can no longer fill. The journal keeps it as it was |
| `REPLAY_HISTORY_FAILED` | A saved replay run's journal cannot be opened, as when it was edited; the message gives the reason |
| `MARKET_HALTED` | A market-wide circuit breaker has halted trading; the message gives the S&P 500's fall and when trading resumes |
| `INVALID_REQUEST` | HTTP 400: a malformed body or query, including an order no market could make valid (see the HTTP errors below); nothing is recorded |
| `UNKNOWN_ACCOUNT`, `ACCOUNTS_UNSUPPORTED` | HTTP 404 for an `account=` the server does not have; 409 when creating an account on a server that keeps one (a replay, or no accounts directory) |
| `LIMITS_REVISION` | HTTP 409: `expected_revision` on `PUT /api/risk/limits` or `/api/risk/guardrails` is not the current `limits_revision`; refetch and retry |
| `TRADING_UNAVAILABLE` | HTTP 503: the command inbox is full or the engine is stopping (with `Retry-After`), or the account's journal failed or is locked |
| `PASS_ODDS_UNAVAILABLE` | HTTP 422: pass odds need an evaluation rule, complete current marks and ten completed days with intraday extremes |
| `INVALID_SETTLEMENT`, `ALREADY_SETTLED` | Invalid/premature settlement or already settled OSI |
| `UNKNOWN_ORDER`, `ORDER_TERMINAL` | Invalid cancellation target or already finished order |
| `INVALID_LIMITS`, `INVALID_TIME`, `INVALID_SCENARIO`, `INVALID_REASON` | Invalid control/configuration input |
| `JOURNAL_IO`, `JOURNAL_CORRUPT` | Persistence stop condition or invalid/tampered recovery chain/schema |
| `JOURNAL_LOCKED` | Journal already owned by another writer; analytics remain available |
| `EVALUATION_CLOSED` | The attempt passed or failed; reset to trade again |
| `BUYING_POWER`, `BUY_ONLY`, `EXPIRY_CUTOFF` | Account-rule rejections (see Account rules); `EXPIRY_CUTOFF` also cancels every open order on a contract at the account's pre-expiry cutoff |
| `ACCOUNT_RESET` | Working order cancelled by a reset |
| `INVALID_RULES` | Negative rule money, a negative cutoff or one of a day or more, a plan name over 64 bytes or one that names a preset whose balance and rules these are not, payout percentages outside 0-100 or nonpositive caps, slippage or impact outside 0-10 ticks, fill latency outside 0-60,000 ms, or a funded phase with a profit target or no qualifying days. Rule values of the wrong type, such as a fractional tick count, are 400 `INVALID_REQUEST` |
| `OCO_FILLED`, `POSITION_CLOSED` | Bracket sibling cancelled when the other exit filled completely, remaining entry cancelled by an exit fill, or an exit whose held legs closed |
| `PAYOUT_UNAVAILABLE`, `PAYOUT_NOT_ELIGIBLE`, `INVALID_PAYOUT` | Not a funded, active account; a payout requirement unmet; or an amount that is not whole cents or outside the minimum and maximum |
| `PLAN_LOCKED` | A funded preset was requested without first passing the evaluation that unlocks it: that preset's own balance and rules |
| `INVALID_NOTE`, `UNKNOWN_TRADE` | An order or trade note or tag past its limits, or a note on a fill that opens no trade |
| `INVALID_GROUP` | An order's `group` names no open round trip or whole trade holding one on its underlying; grouping names a fully closed trade, mixed underlyings or too few round trips; or ungrouping names a closed round trip |
| `DEFINED_RISK` | A defined-risk plan's order, bracket exit or exercise would leave a short option uncovered, now or once the open orders fill |

## Engine integration and HTTP API

Paper trading is enabled by default in `openportd`; `--no-paper` disables it and
reports `PAPER_DISABLED` in status.

### Accounts

The journal at `--paper-journal` is the **main** account. More named accounts live
in an `accounts` directory beside it, one journal each (`accounts/<id>.jsonl`, the
display name in `accounts/<id>.name`). Accounts list after main in ID order
(`swing-50k` before `swing-50k-2`), both when created and when recovered at startup,
so a restart keeps the list as it was. Every
account trades the same market at once: each receives the quotes and valuations for
its own positions, open orders and commands, marks, fills, settles, rolls its day
and applies its rules whichever account the terminal shows. An account whose
journal cannot open, or fails later, reports why in its status and refuses writes;
the others carry on.

`POST /api/accounts` takes a `name` (1 to 64 characters, counted as Unicode code
points, none of them a control character) and either a
preset `plan` or `initial_cash` and complete `rules`, and returns 201 with the new
account's `id`, a slug of the name made unique (`swing-50k`, `swing-50k-2`). Funded
plans are refused: they start by resetting an account that passed the evaluation.
Every other trading route acts on the main account, or on another given as
`account=<id>` in the query (reads and writes alike; a write takes no other query
parameter). An unknown account is 404 `UNKNOWN_ACCOUNT`; a malformed one is 400.
Every route and the token check read query keys and values percent-decoded, so
`%61ccount=beta` is `account=beta` and needs beta's scope. A key given twice in any
spelling, or a malformed escape, is 400.
Without an accounts directory (`--paper-journal` empty in tests) the server keeps
one account, as a replay does, and account creation returns 409 `ACCOUNTS_UNSUPPORTED`,
which a retry cannot change.

### Commands and views

The engine thread alone owns every session. A bounded FIFO inbox (256 pending
commands) sequences writes, applies the drained market batch first, then applies
commands in ingress order. HTTP threads enqueue
and return; completions are posted onto the requesting Beast session executor.
A full inbox or stopping engine returns 503 `TRADING_UNAVAILABLE` with
`Retry-After: 1`: the inbox drains as the engine applies commands, and an order sent
again with the same client ID and terms is answered once.

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
Versions are decimal strings and `write` is `open`, `token`, or `disabled` (see
[write protection](#write-protection)). Clients refetch
portfolio, orders and risk when the version changes. Chain option objects include
canonical padded `symbol`, whole `bid_size`/`ask_size` (null when unavailable),
`tradable` and `untradable_reason`, using the core eligibility policy, and
`executable`: whether paper orders can fill on the displayed quote, a positive,
uncrossed bid and ask with at least one whole contract on each side. When it is false,
`quote_issue` says why (`no_quote`, `no_bid`, `no_ask`, `crossed` or `zero_size`); a
contract can be tradable with a quote that is not executable.

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

Both `/api/status` and WebSocket ticks also include a top-level `circuit_breaker`
object, shared by all accounts and available even with paper trading disabled:

| Field | Meaning |
| --- | --- |
| `symbol` | The watched symbol: `SPX` if subscribed, otherwise `SPY`. No reference is invented when its data is unavailable. |
| `day` | Current trading date as `YYYY-MM-DD` on the market-data clock; null before any data or recovery. As with the paper accounts, the next trading date starts after 17:00 ET. |
| `previous_close` | `{date, price}` for the previous business day, or null when unavailable. The date is `YYYY-MM-DD`; the price is a number. An official close takes precedence over a recorded closing print. |
| `level` | Highest level tripped on `day`, from 0 (none) through 3. |
| `halts` | Halts of the most recent day that tripped one, in start order. Each has `level`, `start`, `end`, `reference`, `price` and `active`. The two prices record the previous close and the print that tripped that halt. |
| `active` | Whether any halt covers the latest market time, including its start and excluding its end. Each halt's flag uses the same clock. |
| `market_time` | Latest observed market time, or recovered time at startup; null before either is available. This and halt timestamps use the API's UTC ISO timestamp format. |
| `error` | Last breaker storage failure for this engine instance, or null. A later successful write does not erase the warning. |

The engine thread publishes this snapshot under the status mutex after each market
batch. The active flag follows delayed or replayed market data, so elapsed wall time
alone does not end the banner. A connected tick replaces the terminal's breaker
state; REST supplies it before the first tick and while disconnected. Older servers
without the object show no banner.

The Brief page shows the selected account's plan, attempt, floor and effective soft
floor, closest recorded floor approach, guardrails, pending changes and open holdings
and orders. Its loss allowance is the smallest of daily-loss limit minus current daily
loss, equity minus the soft floor, and equity minus the plan floor. Disabled limits
are excluded, exhausted allowance is zero, and incomplete or mixed-version valuations
leave it unavailable. These are exact decimal-string calculations for display; the
server still enforces all trading rules. Position deadlines use the contract's last
trade and the selected plan's cutoff, including AM settlement on the following day.

Brief edits the trading day's plan through the regular session and prompts for review
after the close. Session rows identify early closes; without those rows the prompt
uses 16:00 ET. The next engine trading date starts a new plan. Both fields use the
existing day-note route, preserving the other field. Drafts stay local until saved and
are cleared on account, source or date changes. Missing note reads cannot be overwritten
with an empty note. Read-only runs keep the note visible with saving disabled.

The Journal page edits each trade's note and tags (a strategy's apply to each of its
legs), filters every panel by tag and reports P&L by tag. Browser alert settings
for price levels on an underlying and each new fill are kept in
the browser's local storage and run while the terminal is open, on the live feed only
(not a replay). They show on the page and, where the browser allows notifications, as
system notifications, with an optional chime. A price alert fires once, when the
feed's price reaches its level from the side it was set on. Rule alerts use the same
notifications and chime: floor room below 50%, 25% and 10% of the plan drawdown;
daily loss at 50%, 75% and 90% of its limit; a guardrail latch; pending rules taking
effect; and a target within 10%. Each fires once per account and trading
day, including after a page reload. Claims are stored in localStorage with an
in-memory fallback when browser storage is denied.

External notifications run on the server while the browser is closed. Live account
publications supply new fills, rejected orders, kill and daily-loss trips, assignments,
exercises and staged playbooks. The engine also detects stalled feeds without an API
reader. Each channel can warn when `breach.room` is at or below its dollar distance;
missing room or incomplete account valuation supplies no price estimate. Channels
rearm when room moves above the distance. Recovered fills and deliveries are history
and are not sent again. Replays, drills, scenarios and backtests cannot send.

The observer and delivery queue do not change reducer inputs or journal records.
Messages label trading as paper/simulated and use the event's market time. Delivery
timing uses a separate clock. The Notifications section in Alerts settings shows
channels, editable filters and a test button; destinations and credentials stay on
the server. See [external notifications](runtime.md#external-notifications) for setup,
delivery guarantees and limits.

The web ticket estimates fees using `fee_per_contract`; only older servers without
it expose a manual fee estimate. Ticket and Positions notices use `paper.message`,
and `paper.accepting: false` disables ticket submission. In the overnight and curb
sessions (by `paper.session`) the tickets offer limit orders only, with a condition or
bracket only on a GTC limit, which waits for the regular session, and Close all is
disabled because flattening sends market orders. The
flatten dialog names an underlying whose paper orders are refused (a stalled feed, a
halt), whose positions and orders the flatten leaves, and does not count its orders
among those cancelled; Close all is disabled when that is every underlying in scope.
After a flatten, its dialog lists each closing order's outcome (a close still
working says so), the delivered shares it traded and any it left with the reason, and
each position still open with the contracts being worked and why the rest stay. The
dialog counts the orders it cancels without the bracket exits, which stay until the
position they protect is flat. For
older servers without `paper`, they fall back to the session-based notice and
submission gate.
Limit prices display cents, with buttons and arrow keys following the root's tier
tick table above (including downward steps across $3.00). Typed off-tick prices
still receive the server's `INVALID_TICK` reason. On wide screens the ticket docks
beside the chain; elsewhere it is a dialog. It names the strategy from the held
position (Long Call, Close Short Put...), sets the limit from Bid/Mid/Ask, says whether
the order is marketable at the far side or will rest, and requests buying power and
floor risk from the server preview. The web has no second copy of margin rules.
Quoted premium estimates exclude slippage; the server preview includes the plan's
slippage. Buy-only plans and decided attempts block submission with the reason.

Strategy mode's **Templates** menu selects verticals, iron condors, iron butterflies,
strangles, straddles, long call or put butterflies, calendars and diagonals. Delta
selection uses the chain's per-strike deltas. Forward offsets use points or the
expiry's expected move, F × ATM IV × √T. Positive offsets go out of the money;
condors use both sides. Verticals target the short leg for credit and debit spreads.
Calendars and diagonals sell the selected near expiry and buy the chosen far expiry,
whose full chain must load first. A diagonal's signed offset sets its far strike
relative to the near strike. Calendars require the same listed strike in both chains.

Targets snap to listed strikes in the loaded chain, with ties going to the lower
strike. Targets outside that range, missing quotes or deltas, overlapping legs and
unequal butterfly wings give a reason instead of an order. Widen the chain window
when needed. Review shows the legs, actual widths, net credit or debit, max loss and
probability of profit before submission. Calendars and diagonals retain the ticket's
estimated first-expiry risk, valuing later legs at today's IV. Named presets are
stored per underlying in this browser; the selected near expiry remains the near
expiry when a preset is reused. Each template exposes a parameter tag, displayed in
the ticket and cleared on manual leg edits. It goes with the order as a tag, so the
trade it opens carries it into the Journal's reports by tag.

Single-leg and strategy tickets show risk-neutral probability of profit before fees,
using the distribution of the underlying at the order's first expiry. They prefer
that expiry's raw SVI fit when its butterfly and calendar checks pass, then its SSVI
slice, then the chain's smile. The digital probability is the strike derivative of
the smile-priced call, N(d2) − φ(d2)√T ∂σ/∂ln(K/F). SVI and SSVI use analytic
volatility derivatives. Chain IV is interpolated in log moneyness; skew comes from a
local least-squares fit over up to five neighbouring strikes. Without a smile the
ticket labels the flat-IV estimate “lognormal, no smile”. No usable volatility means
no probability estimate.

Outside the fitted or quoted range, volatility stays flat and skew is zero. A fixed
log-strike grid, including quoted knots and both sides of the range endpoints, clamps
the digital to [0, 1] and takes its running minimum. Interpolation of that grid gives
non-increasing probabilities at every strike; flat smiles retain N(d2) exactly.
Any repair is labelled approximate, and breakevens outside the range disclose flat
volatility. These are model probabilities, not forecasts of realised returns.

After HTTP 400/403/404/409/422, the ticket shows the rejection details and **New order**,
which preserves form values and starts a fresh client ID. **Retry same order** keeps
the frozen request and ID only after a network failure, timeout or 503. Other
unexpected responses direct the user to check Positions and Orders. Results receive keyboard
focus at the top of the ticket.

| Endpoint | Request / response |
| --- | --- |
| `POST /api/positions/exercise` | Canonical `symbol` and positive `quantity` of long equity or ETF contracts to exercise early; returns the portfolio |
| `POST /api/positions/abandon` | Canonical `symbol` of a long nobody bids for, or one awaiting settlement, to give up at zero without a fee ([disposal](#disposing-of-worthless-positions)); returns the portfolio |
| `POST /api/positions/instruction` | Canonical `symbol` of a long option and boolean `do_not_exercise`: true makes it expire worthless at settlement, false withdraws that; returns the portfolio, whose positions carry `do_not_exercise` and `no_bid` |
| `POST /api/stocks/close` | `symbol` of delivered shares (`SPY`) and optional positive `shares`, all of them when left out; closes at the underlying's price in the regular session and returns the portfolio |
| `GET /api/portfolio` | Account cash, equity, daily baseline/P&L, realised/unrealised, fees, completeness/quality flags, marked positions and Greeks, delivered `stocks` (symbol, shares, average price, basis, mark and its time, market value, unrealised and realised P&L, fees, freshness and today's attribution), and today's `attribution` (`delta`, `gamma`, `vega`, `theta`, `other`, `costs`, `total` in dollars) for the account and each position (null until the position's next fill or rollover), and `liquidity_used`: the current quotes whose displayed size the account's orders have taken some of, with each side's size and what is left (`bid_left`, `ask_left`) until a new quote |
| `GET /api/orders?status=all` | All orders, newest first; `status=open` restricts to working, partially filled and armed orders |
| `POST /api/orders` | `client_order_id`, canonical `symbol`, `side` (`buy`/`sell`), `type` (`limit`/`market`), integer `quantity`, decimal-string `limit_price` for limits, `time_in_force` (`day`/`gtc`/`ioc`), optional `tags` and `note`, optional `trigger` `{source: option\|combo\|underlying, direction: at_or_below\|at_or_above, level}` and `bracket` `{stop_loss?, take_profit?}` whose exits each take one of `trigger` or `limit_price`. A multi-leg order replaces `symbol` and `side` with `legs` (two to four `{symbol, side, ratio?}`, ratio default 1, or up to eight for a roll), allows an entry bracket or a reducing trigger (combo or underlying), counts units in `quantity` and sets a signed net `limit_price` (negative for a credit); `exits_only: true` attaches a bracket to held closing legs as described above; optional `group` joins what the order opens to a [whole trade](#whole-trades); 201 returns version, order and its fills. Orders report `legs` (null for single-leg), with null `symbol` and `side` for multi-leg orders. Retrying with a `client_order_id` already used and the same terms is safe: it returns the first answer (200 with the order as it now stands, also after `PUT /api/orders/{id}` changed it, or the original rejection) and records nothing, while other terms under that ID, the changed ones included, reject with 409 `DUPLICATE_CLIENT_ID` |
| `POST /api/orders/preview` | The order body plus optional `floor_share` (default 0.5); 200 returns the dry-run decision, buying power, exposure change, labelled maximum loss, floor warnings, `max_units` with its buying-power and floor parts (null when unavailable), projected `breach`, and `execution`: what submitting now would fill at once and the full size's fill schedule, and each leg's quote `liquidity` |
| `POST /api/orders/what-if` | `candidates`: one to six, each an optional `name` and one to four `orders` as submission takes them (client ID optional); 200 returns the account `current` and each candidate's `decision`, `reason`, per-order `orders` checks and the account `after` its orders fill in full (null when one cannot be projected): equity, buying power, exposure, grid max loss, floor flags, scenarios and breach ([what-if](#what-if)) |
| `GET /api/account/equity?from=&to=` | Persisted equity samples with optional inclusive UTC ISO bounds, plus any storage error |
| `DELETE /api/orders/{id}` | No body; 200 returns version and resulting order |
| `PUT /api/orders/{id}` | Any of integer `quantity`, decimal-string `limit_price` and `trigger_level`; 200 returns version, the changed order and its fills (see [changing orders](#changing-cancelling-and-flattening)) |
| `POST /api/orders/cancel` | Optional `underlying`; cancels every open order, or that underlying's, and returns version and `cancelled_orders` |
| `POST /api/positions/close` | Optional `underlying`; cancels the open orders in scope but the bracket exits and closes its positions at market with reduce-only orders that keep working until filled, returning version, `cancelled_orders`, the closing `orders` (each with its status and reason), their `fills`, the delivered shares it closed (`stock_fills`) and those it could not (`kept_stocks`: symbol, shares and reason), and each position still open (`residuals`: symbol, underlying, signed `quantity`, the contracts still `working` and the `reason` the rest are not, or null). 422 with the reason, and nothing changed, when nothing in scope can close ([flattening](#changing-cancelling-and-flattening)) |
| `POST /api/positions/close/preview` | Optional `underlying`; the flatten's dry run on a private copy: `decision` and `reason`, `cancelled_orders`, the closing `orders` without IDs and their `fills`, `stock_fills`, `kept_stocks`, `remaining` and `remaining_shares` in scope, and the account `current` and `after`; `simulated: true`, nothing recorded ([flattening](#changing-cancelling-and-flattening)) |
| `GET /api/fills` | Version and fills, newest first, with pre-execution `context` and the `quote` each took: `observation`, `bid`, `ask`, `bid_size`, `ask_size`, `size_left` (displayed size still free for paper orders before the fill), `quoted_at` (when the quote was first given) and `age_seconds` (both null on older fills) |
| `GET /api/trades.csv`, `GET /api/fills.csv` | CSV downloads with `account`, inclusive New York `from`/`to` dates, fixed columns and exact money; see [CSV downloads](#csv-downloads) |
| `PUT /api/days/{YYYY-MM-DD}/note` | Required `plan` and `review` strings replace the day note; returns version, `day` and `note`. Invalid text returns `INVALID_NOTE` (422); invalid dates return 400 |
| `GET /api/risk` | Version, active/pending limits and guardrails, guardrail progress, pending activation, daily loss, kill state, aggregate/underlying buckets, scenario matrices and `breach` |
| `PUT /api/risk/limits` | `expected_revision` string and complete `limits` object; tighter fields apply now, looser evaluation fields are pending until rollover; 200 returns the risk view, 409 `LIMITS_REVISION` if the revision changed (refetch it and retry) |
| `PUT /api/risk/guardrails` | `expected_revision` string and complete `guardrails`; tighter fields apply now, looser fields wait for rollover on all accounts; returns the risk view, or 409 `LIMITS_REVISION` as for limits |
| `POST /api/risk/kill` | `action` (`trip`/`reset`) and nonblank `reason`; returns version, kill state and cancelled order IDs |
| `POST /api/settlements` | Canonical `symbol` and decimal-string `value` for an expired AM position, or a PM one whose closing print never arrived (its `settle_by` is `manual`); returns version and `position_closed` |
| `GET /api/account` | Rules (including `phase`, `lock_balance` and `payouts`), personal guardrails and progress, `breach`, evaluation (attempt, status, starting balance, equity, `marked`, profit, peak, floor, `floor_locked`, drawdown buffer, target equity/remaining (`0.00` once passed, though liquidating at the bid can leave equity just below the target), decision, current day, finished `days[]` with `realised`, `qualifying`, `attribution` and equity low/high with times, attempt closest-floor distance/time, `qualifying_days`, `cycle_started` and `payouts[]`), buying power, `payout` (the next payout's standing from `payout_quote`: `eligible`, `blocked`, number, flat/active, qualifying and required days, profit, withdrawable, cap, maximum, minimum, trader share and percentages; null outside the funded phase) and earlier `attempts[]`; absent rules give null floor/target |
| `GET /api/trades?status=open\|closed\|all&attempt=current\|all` | Round trips, newest first: direction, status, opened/closed/duration, quantities, average open/close, cost (entry premium), gross, fees, net, `return` (net / cost, closed only), mark/unrealised while open, `closure` (`settlement` at expiry, `exercise` for an early exercise, `assignment` for an early assignment, `abandon` for an abandoned long, `reset`, or null when fills closed it or it is open), fill IDs, attempt, and the trader's `note` (`""` for none) and `tags`, `entry_context`, `exit_context`, `review`, `strategy_id`, `strategy_review`, the whole trade it is in, `group`, and `buying_power`, `return_on_buying_power`, `strategy_buying_power` and `strategy_return_on_buying_power` (see [trade review](#trade-review)); `groups` lists the [whole trades](#whole-trades) with more than one entry. `day_notes` holds the account's daily plans and reviews. Defaults: all statuses of the current attempt. `stock_fills` lists every change in shares (`id`, `symbol`, signed `shares`, `price`, `time`, `source`, `option`) and `dividends` every dividend paid (`symbol`, `ex_date`, `per_share`, signed `shares`, `amount`, `time`), oldest first, which the terminal announces when new. `share_trades` lists the shares' round trips the same way (`kind: "shares"`, `id` `s` + the opening stock fill, shares instead of contracts, no fees), with `opened_by`/`closed_by` (`expiry_exercise`, `assignment`, `early_exercise`, `trade`, `rule` or `reset`) and the `option`/`closing_option` that delivered them |
| `POST /api/trades/group`, `POST /api/trades/ungroup` | `trades`, round trips by trade ID: join their trades into one (a closed round trip can name a whole trade still holding an open one), or take each listed open round trip out of its trade (see [whole trades](#whole-trades)). Returns version and `groups`, the trade each named round trip is in now; `UNKNOWN_TRADE` (404), `INVALID_GROUP` (422) |
| `PUT /api/trades/{id}/note` | Optional `note` string and `tags` array replace the trade's (see [trade notes](#trade-notes-and-tags)); an empty note with no tags clears them. The `id` is a trade's, or a share trade's (`s` and its opening stock fill). Returns version, `trade`, `note` and `tags`; `UNKNOWN_TRADE` (404) if no trade opens with that fill, `INVALID_NOTE` (422) for text past the limits |
| `GET /api/plans` | Presets: `practice` (buying power only), `intraday-25k/50k/100k` (buy-only, 10% target, 5% intraday trailing), `eod-25k/50k/100k` (any side, 12% target, 6% end-of-day trailing) and their `funded-*` accounts (`unlocked_by` names the evaluation); evaluations and funded accounts auto-close five minutes before the last trade (15:55 ET for SPXW, 16:10 for SPY) |
| `POST /api/account/reset` | Nonblank `reason` plus either a preset `plan` ID, or `initial_cash` and complete `rules` (optional `phase`, `lock_balance`, and `payouts` required exactly when funded); returns the new account view. Funded presets need a passed matching evaluation (`PLAN_LOCKED`) |
| `POST /api/account/payout` | Decimal-string `amount` in whole cents; returns the account view with the recorded payout |
| `GET /api/accounts` | `accounts`: each account's `id`, `name`, `trading` status and `equity`, the main one first |
| `POST /api/accounts` | `name` and a preset `plan`, or `initial_cash` and `rules`; 201 returns the new account's `id`, `name`, version, plan and equity (see [accounts](#accounts)) |

Every route in this table except `/api/plans` and `/api/accounts` takes `account=ID`
in its query for an account other than the main one (see [accounts](#accounts)).
`/api/replay` and the routes under it serve a replay of a recording or the simulated
demo market; see [replaying in the terminal](runtime.md#replaying-in-the-terminal).

Rules JSON is `{plan, profit_target, max_drawdown, drawdown_mode, buy_only,
defined_risk, slippage_ticks, fill_latency_ms, impact_ticks, margin, buying_power, expiry_cutoff_seconds}`.
`defined_risk`, `slippage_ticks`, `fill_latency_ms`, `impact_ticks` and `margin` are optional when creating or resetting
an account: `defined_risk` defaults to false, the execution settings to 0, and
`margin` to `"strategy"`. Older journals missing these
fields recover with the same defaults. Money is null for a disabled target or
drawdown and `drawdown_mode` `intraday` or `end_of_day`. Portfolio adds
`buying_power: {available, reserved, short_requirement, requirement}` (`requirement`
is the same amount as `short_requirement`, named for both margin modes) and `margin`,
the requirement by underlying with what holds it (see buying power under Account rules); orders add `origin`
(`user` or `system`), `tags`, `note`, `exits_only` (false when absent in older journals), `status` `armed`, `trigger`, `triggered_at`, `bracket`, `role`
(`stop_loss`/`take_profit`/null), `parent`, `oco`, `stop_loss_order`,
`take_profit_order`, `ended_at`, `modified_at`, `changes` and `waiting` (see below); status and ticks add `trading.plan` and `trading.evaluation`
(`active`/`passed`/`failed`, null without a target or drawdown rule). `--plan ID`
chooses the rules for a new journal (default `practice`); `--paper-cash` then overrides
its starting balance. Recovery keeps the recorded rules. New latency and impact fields default to zero
when absent and are omitted from account responses and journals at zero. Pending
orders recover their acceptance/trigger clocks and consumed depth.
`POST /api/account/reset` and `POST /api/accounts` accept optional
`fill_model: "as_displayed" | "conservative"` beside the plan or custom rules. It
overrides only latency, impact and slippage for that account's new attempt.

Money is an exact decimal string, quantities are integers, IDs/versions are strings,
and timestamps use the same UTC ISO format as `as_of`. Analytical values may be
null. Position Greeks expose per-unit delta/gamma/vega/theta plus signed position
dollar exposures. Incomplete scenario grids contain null P&Ls, never partial sums.
Limits contain `max_order_contracts`, `price_band_absolute`, `price_band_relative`,
`aggregate` and `per_underlying` (`dollar_delta`, `vega`), `max_daily_loss`,
`max_quote_age_seconds` and `max_valuation_age_seconds`.

Unknown fields, duplicate JSON keys, missing required fields, wrong types and
noncanonical OSIs return 400 `INVALID_REQUEST`. So does an order no market could make
valid, whichever field breaks it: an empty, overlong or control-character client ID,
a quantity below one, a market order that is not IOC, a single contract's limit that
is not positive, fewer than two or more than eight legs, two legs naming one contract,
or a ratio outside 1 to 10. Nothing is recorded and the client ID stays free. Business
rejections, which depend on the account and the market, return 422 and remain recorded
as rejected orders; unknown contracts/orders return 404,
terminal orders and a client ID reused with other terms return 409; an identical
retry returns 200 with the order as it now stands. Errors always have this shape:

```json
{"error":{"code":"DELTA_LIMIT","message":"...","actual":1250000,"limit":1000000,"scope":"SPX"}}
```

Unused `actual`, `limit` and `scope` are null. `scope` names an underlying or
`aggregate`: a check on one contract, such as `PRICE_BAND`, reports that contract's
underlying, while the journaled decision keeps the contract's OSI symbol, as the
order itself does. Rejected writes still consume their client ID; GET orders shows
their resulting rejection reason. An order's `reason` has the same shape as an error,
`{code, message, actual, limit, scope}`: a `RISK_CHANGED` cancel keeps the original
check's code at the start of its message and its numbers (3 contracts against a new
limit of 2, a projected loss of 15.65 against 15.64), and so does a Flatten leg the
rules refused. The preview's `reason`, a payout's `blocked` and a flatten's
`kept_stocks[].reason` use it too. The Orders page and the flatten dialog show the
numbers beside the reason.

Orders also report `ended_at` (null while open), `modified_at` (when a change last
applied, or null), `changes` (each with `time`, `actor`, the requested `quantity`,
`limit_price` and `trigger_level`, null where kept, the `previous` terms, `applied`
and the refusal's `reason`) and, while open, `waiting` (`{code, message}` from the
table under [changing orders](#changing-cancelling-and-flattening), or null). The
Orders page's **Details** shows an order's history: accepted, triggered, each change
and refusal, and how it ended or what it waits for.

### Write protection

Every request, read or write, HTTP or WebSocket, must name the server in Host by an IP
address, `localhost` or a name under `.localhost`, the host of an `--allowed-origin`, or
a name given with `--allowed-host`; others get 403 `HOST_REJECTED`. DNS rebinding points
a name its attacker controls at the server, which makes that name's pages same-origin
with it, and that name is what their requests carry in Host.

Every POST/PUT/DELETE under `/api/` uses the same protection. POST and PUT require
`Content-Type: application/json` (an optional media-type parameter is accepted);
DELETE has no body. The body limit remains 64 KiB. A present Origin must match Host
or an exact `--allowed-origin`; invalid or ambiguous security headers fail closed
with 403 `ORIGIN_REJECTED`. Non-browser clients may omit Origin.

`--write-token TOKEN` overrides `OPENPORT_WRITE_TOKEN`. When configured, all writes
require `Authorization: Bearer TOKEN`, checked with a constant-time digest comparison;
missing/incorrect credentials return 403 `WRITE_TOKEN_REQUIRED`. Without either,
`--write-token-file PATH` uses the token kept in PATH, creating a random one with
owner-only permissions when it is missing, and prints a link carrying it,
`http://localhost:PORT/#token=TOKEN`, which the terminal saves in that browser tab and
drops from the address bar; the Docker image keeps its token in its volume this way.
The terminal's "Enter write token" button saves a token by hand.
Without a token, only loopback binds allow writes. Non-loopback binds return 403
`WRITE_DISABLED`.

Status reports the result as `write`: `open` when a write without credentials is
accepted (a loopback bind with no legacy token, `--require-token` or `--sandboxes`;
named tokens alone leave it open), `token` when writes need a token, and `disabled`
when no credential can write (a non-loopback bind without tokens). A token sent to an
`open` server is still checked and names the actor.
Named tokens come from `--token-file FILE`, one `NAME SCOPES SECRET` per line.
Scopes are comma-separated. Blank lines are ignored and `#` starts a comment.
Malformed lines, duplicate names, secrets or scopes, unknown scopes and reserved
actor names fail startup. Diagnostics name the line, never its secret. The file is
read only at startup; protect it and restart the server to rotate credentials.

| Scope | Permission |
| --- | --- |
| `read` | Every API GET, CSV export and WebSocket ticks |
| `trade:ACCOUNT` / `trade:*` | Orders and previews, cancels, flatten, exercise, stock closure, notes and sending or dismissing playbook stages on the named account / all live accounts |
| `replay` | Start, control and stop replays, and trade their isolated accounts |
| `admin` | Everything, including limits, guardrails, kill switch, resets, payouts, settlements, account creation, playbook definitions and modes, and replay history deletion |

The legacy write token has `admin` scope and actor name `legacy`. Writes always
check the token they carry: an unknown one gets 403 `WRITE_TOKEN_REQUIRED`, and a
valid one without the required scope gets 403 `SCOPE_REQUIRED`. Unrecognized route
families require `admin` for writes.

Without `--require-token`, reads remain public: any caller reads, and a read ignores
a token that matches nothing, such as a stale one saved in a browser tab. Loopback
writes without credentials remain open, including when a named-token file is loaded,
unless a legacy token, `--require-token` or `--sandboxes` closes them. Visitor
sandboxes close them because a public demo often sits behind a reverse proxy on the
same machine, where every visitor would arrive as a loopback writer; the operator
then trades with the legacy token (for example from `--write-token-file`) or a named
one. With `--require-token`, reads and writes require credentials even on
loopback, and reads need the `read` or `admin` scope; named tokens do not imply
`read`, so combine it with trade or replay scopes. Startup refuses the flag without
a configured token. Static terminal files remain public so the
browser can load the token entry screen. The terminal sends its saved token on
reads and CSV downloads. Browser WebSockets use the `openport` subprotocol plus
`openport.token.HEX`, where HEX encodes the token bytes; the server selects only
`openport`. Non-browser sockets can send the normal Authorization header. Tokens
never go in a URL query. Use HTTPS at your reverse proxy for remote credentials
and configure its public Origin with `--allowed-origin` when it rewrites Host.

### Visitor sandboxes

With `--provider demo --sandboxes N`, `POST /api/sandboxes` creates an isolated
practice account without the operator's token. Its temporary token has `read`
and `trade:ACCOUNT` scopes, restricted to that account and the simulated market.
Other clients cannot list or read it; authenticated admins can. It cannot use
admin routes, playbook actions, replays or backtests. The response returns the
secret once. Sandbox tokens, accounts and files expire after 24 hours unused and
are removed on restart. An expired sandbox token returns 403 `SANDBOX_EXPIRED`.
The reducer and journal schema are unchanged. See [runtime](runtime.md#public-sandboxes)
for proxy headers, limits, storage and configuration.

### Actors

Each new journal transaction includes `actor` in its hash-protected payload: a
named token, `legacy`, `loopback`, or `system` for reducer actions such as market
matching, expiry, rules and assignment. The transport supplies the actor on the
queued `TradingCommand`; request JSON cannot choose it. Replay command provenance
also retains it, so verification uses the original actor.

Orders retain the actor that placed them. Fills and bracket children retain that
originating actor even when a later market transaction executes them. A cancel,
modification or note records its own caller on that transaction, without changing
the order's originating actor. Rule liquidation orders have actor `system`.
`GET /api/orders`, `GET /api/fills` and fills CSV expose actors; the Orders page and
trade review show them in small text. Earlier journals and orders default to
`unknown`; compaction preserves existing actor fields. Replay verification omits
the new fields when reproducing hashes from a run recorded before actors existed:
one whose first record has no actor and whose start input names no driver version.
That choice holds for the whole run, so a record whose actor was removed from an
attributed run is a differing transaction.

The [OpenAPI contract](openapi.yaml) describes response schemas, write bodies and
account selection. The [Python and MCP clients](../python/README.md) use these
same routes. All execution remains simulated.

### Durable startup and settlement sources

`--paper-journal PATH` defaults to `$HOME/.openport/paper-journal.jsonl`; containing
directories are created, and named accounts keep their journals in `accounts/` beside
it. `--paper-cash` defaults to `100000` and `--paper-fee` to
`0.65`. These seed new journals; recovery restores the recorded configuration. The
fee also seeds every new named account and visitor sandbox, whose cash and rules
come from their plan.
Existing files are verified, exclusively locked and resumed; an empty one holds no
transaction and starts the main account afresh (see
[Journal, recovery and failure handling](#journal-recovery-and-failure-handling)).
A corrupt, torn, locked
or unwritable journal disables its account's writes with 503 `TRADING_UNAVAILABLE`
and a status reason; other accounts carry on. They are never overwritten or silently replaced by an ephemeral
account. A runtime journal failure preserves the last committed account and
requires operator recovery. The Docker image journals in `/var/lib/openport`,
which is a declared volume; mount a persistent volume there.

At expiry, PM positions settle on the underlying’s **official close** when the
provider publishes one (`md::UnderlyingClose`): Cboe’s close field after the close,
which stops at the closing price while the price goes on with after-hours trades, and
which Cboe may revise within minutes (on 2026-09-24 SPY’s went from 767.27 to 767.18
ten minutes after the close, while the first print after 16:00 was 767.26). A revision
replaces the recorded close until the positions settle. Without one they settle on
the **first positive finite last print stamped at or after the regular close (16:00
ET, 13:00 early) on the expiry date**, in provider arrival order, the provider’s
closing print. ETF options that trade until 16:15 settle then, on that close, as OCC
exercises on the closing price. Bid/ask midpoints and next-day prices are not
substitutes.
Each account records the print in its journal (`record_close`, kept per underlying
and date) as soon as it holds a PM position expiring that day, so a restart before
ETF options expire at 16:15 still settles them on it. If none has arrived half an hour
of market time after the close, the underlying's last print before the close stands in,
provided it came within the close's last five minutes (a feed that stopped just before
the close); the engine keeps both prints in memory for a week of dates, so after a
restart only a print that arrives counts. Until then an expired PM position's
`settle_by` reads `closing_print`, as it does once the print is recorded; after it,
with neither, the position waits and its `settle_by` reads `manual`. AM positions
always wait for an
explicit `/api/settlements` import (the terminal's Settle button on the position),
whose value must come from the authoritative settlement source; PM imports are
accepted only while no closing print is recorded. The same journal transaction
records the reference value, canonical definition and integration
`settlement_source`: `provider_official_close`, `provider_closing_print` or
`provider_last_print_before_close` with provider and time, `manual_am_import` or
`manual_pm_import`. Preserve the official source used for an AM import
externally when an independent provenance audit is required. The first market
batch on a later trading date rolls the daily baseline on the finished day's closing
marks before its own quotes are applied. When a PM position of the finished day is
still to settle, or the close was not completely marked, the day instead rolls over
after that batch's quotes, once the position has settled and the marks are complete,
so the finished day's P&L by Greek and that night's assignments then follow those
quotes; the kill latch survives. All accounting uses effective market time, including
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
compaction; overnight and curb sessions; trade notes and tags; P&L by Greek; exercise,
assignment and delivered shares. The CLI tests compact journals from earlier builds with `openportd`.

Engine and HTTP tests reuse that fixture for resting fills, cancellation, kill/limits,
JSON errors, write protection, restart recovery, AM/PM settlement, named accounts and
replays. Socket tests cover asynchronous POST/DELETE responses and shutdown of pending
commands. A dividend feed, trade-through matching and portfolio
margin remain outside v1.
