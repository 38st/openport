# Features

Everything OpenPort does today. The [README](../README.md) has the short version;
[paper trading](paper-trading.md) documents every rule, the HTTP contract and the
simulation's limits.

## Market analytics

- **Chain**: bid, ask and mid with bid/mid/ask IV, delta, gamma, vega, theta, vanna and
  open interest and the session's volume per strike, with each side's spread and a
  liquidity cue, centred on the money, with the provider's own IV alongside where it
  publishes one. SPX (AM-settled) and SPXW (PM-settled) expiring on the same day stay
  separate. Missing quotes, volume and open interest show as missing, never as zero,
  with coverage counts per expiry. The chain opens on the nearest expiry before its
  auto-close and counts down to it on the market-data clock.
- **Smile and term structure**: out-of-the-money smile per expiry with its SVI fit and
  arbitrage checks, and the ATM term structure on a square-root-of-time axis, with each
  expiry's forward and rate and where it came from.
- **Volatility metrics**: model-free IV and ATM term structures, delta risk reversals
  and butterflies, realized volatility and cones, the variance risk premium, and
  implied session moves with optional event labels. Local minute and close history
  adds IV rank and percentile, labelled index proxies, and ex-post variance risk
  premium for SPX (SPY by proxy). Values use current market time;
  missing, truncated and proxy estimates are marked. [Definitions](volatility.md).
- **Exposure**: GEX and VEX by strike and expiry, total gamma profile, gamma flip, and
  call and put walls.

## Trading terminal

- **Brief**: prior-session and observed overnight levels, implied moves and bands,
  volatility changes, positioning and supplied events for the selected underlying,
  beside the account's floor room, loss allowance, guardrails and open positions.
  Plan before the open and review after the close on market time, including replay.
  Key levels can be drawn on Trade's chart, with the choice saved in the browser.
- **Trade**: the chain with an order ticket docked beside it, and a candle chart of the
  underlying (one-minute to daily, backfilled from Cboe's free history) showing your
  strikes, armed triggers and the selected expiry's expected move.
- **Orders**: DAY, GTC, EXTO, GTC_EXTO and timestamp-bounded GTD limits, market orders, orders that wait for a price level,
  stop-limits, and brackets whose stop-loss (a stop or a stop-limit) and take-profit
  cancel each other, keeping a partly filled position protected. GTC orders wait
  outside the regular session and last until the contract's last trade or auto-close.
  Working orders change in place: size, limit or trigger level. Each order keeps its
  history (accepted, triggered, every change and refused change, and when and why it
  ended, with the check's numbers), and a working order says what it is waiting for:
  its limit, trigger, the regular session, a fresh quote or fill latency. Entry notes and tags
  follow the trades into the Journal. Tickets warn on thin liquidity without blocking;
  market orders show the displayed size they take against. The chain and tickets mark
  quotes paper orders cannot fill on (one-sided, crossed or sizeless) and the displayed
  size your orders already took, so a partial IOC is no surprise. Every fill keeps the
  bid, ask and sizes it traded against, the displayed size left, and how old the quote was.
  EXTO/GTC_EXTO protection works overnight and curb with simulator-managed limit
  execution. Flatten can use limits that follow the touch across quotes in those
  sessions, preserving combo closes, size splits and bracket protection.

- **Strategies**: up to four legs (spreads, straddles, condors, butterflies, calendars
  and diagonals) picked on the chain, or built from a template such as a 10-wide put
  spread at 15 delta or a condor one expected move out, with presets saved in the
  browser. Legs fill together at a net debit or credit. The ticket shows the P&L at
  expiry and today, the expected move, and the probability of profit from the smile's
  risk-neutral distribution, skew included. A held strategy is one row with its net
  P&L and Greeks, closed or rolled in one order: to a later expiry, to new strikes in
  the same expiry, or a whole condor as one eight-leg order. A roll or an adjustment
  stays one whole trade with its own review. Spreads take OCO
  exits on entry or while held, with stops on the closing net or the underlying, and
  a condor or iron butterfly can roll its put or call vertical as one four-leg order.
- **Evaluations**: a profit target and a trailing drawdown floor (intraday or end of day)
  decide pass or fail, with buy-only, defined-risk and buying-power rules and auto-close
  before expiry. Limits tighten immediately; looser evaluation limits wait for the
  next trading day. Personal soft floors, trade limits, cooldowns and profit locks
  keep the account reduce-only when reached.
  The Dashboard charts minute and fill equity against the target and floor, and the Journal keeps a P&L
  calendar, win rate, profit factor, return on buying power and reports by hold time,
  weekday, month and tag,
  per contract or per strategy, with shares from exercise and assignment as trades of
  their own. Each trade's review shows the market and the account at entry and exit,
  how far it went against you and in your favour (MAE and MFE), what you gave back and
  its R-multiple and its P&L by Greek over its life, on the underlying's chart, and
  marks the exits a stop, a flatten or a liquidation forced. Trades take notes and tags, each day a plan and a review, and
  trades and fills download as CSV, each row naming its attempt and, from a replay,
  the run, scenario and seed. A new attempt keeps the history.
- **Risk**: Greeks per position, today's P&L split by delta, gamma, vega and theta
  (with costs apart, and contracts closed today still listed), dollar-delta and vega limits, a spot × volatility scenario grid,
  a daily loss limit and a reduce-only kill switch: closing orders and bracket exits
  keep working while opening orders are cancelled. The latch keeps the history of its
  trips, resets and releases, and says when a reset can clear it. Positions close together as one
  order, or flatten an underlying or the whole account at market: each short closes
  together with the long that covers it, so no short is left naked; closes larger than
  the order limit split, what a thin quote leaves works on later quotes until flat,
  bracket exits keep protecting whatever is still open, and the flatten reports what
  stays and why. A short quoted 0.00/0.05 is bought back at its ask, a long nobody
  bids for is abandoned at zero without a fee, and a long option can be
  told not to exercise at expiry. Server order
  previews show buying power, maximum loss, what fills at once and the full size's
  fill schedule, and the size that fits buying power apart from the size that uses at
  most half the floor room; Edit order previews a change to a resting order the same way,
  and Flatten shows its dry run, what it would cancel and close and the account after it.
  Warnings name what the held book is close to: a delta or vega limit (by gamma, too),
  the soft floor, tonight's floor ratchet, delivery of in-the-money ETF options at
  expiry, early assignment and ex-dates. What-if compares candidate adjustments, each
  of up to four orders, against the held book on buying power, Greeks, grid loss and
  floor room before any is sent. Breach risk shows the spot moves that could reach the floor (the
  personal soft floor on an account without a plan floor) and labelled model
  estimates of touching it before the close.
- **Playbooks**: versioned setups with entry windows and conditions, shared strategy
  templates, preview sizing and spread exits. Trade, Brief and Dashboard stage orders
  for review; replays and scenarios can send them automatically. The Playbooks page
  compares adherence and expectancy, and the Journal filters by playbook. Dashboard
  and playbook pass odds are labelled estimates from past results, not predictions.
  [Rules and limits](playbooks.md).
- **Batch backtests**: run a pinned playbook over recorded, imported or seeded
  scenario days, with independent daily results and carried-account evaluation
  attempts. The CLI, API and Backtest page keep reports and verifiable journals.
  Results are simulated trading, not predictions or investment advice.
- **Accounts**: several named accounts at once, say a 50K evaluation beside a practice
  book, each with its own journal, rules and positions on the same market. Public
  demo visitors can create private sandbox accounts on simulated prices, removed
  after 24 hours unused or a server restart.
- **Replay**: record every session and trade any recorded day again beside the live
  feed, on a practice or evaluation plan, at 1× to 300× or as fast as possible. Start
  at a chosen New York time, pause or skip, and keep each run's trades in its own journal.
  Resume a run a crash interrupted, on the same journal, where it stopped.
  Finished runs open read-only in Journal and Dashboard. Replay journals sync to disk
  every 250 ms rather than every record, so a power cut can lose their last quarter
  second.

  - **Reproducible runs**: playback speed leaves batches, fills and journals unchanged.
    Step to a market time with `PUT /api/replay {"until":"10:30"}`, or a date and time
    in the terminal; check a saved run with `openportd --verify-run JOURNAL`.
  - **Multi-day runs**: scenarios of several regular, curb and overnight sessions over
    several days play as one steppable run on one account, with the rollover, the
    end-of-day ratchet, GTC carry, gaps at the open, early assignment and dividends.
- **Historical days**: import a past trading date from Databento or your local
  Theta Terminal into a private recording, then trade it in Replay. Imported days
  show their provider and an imported label. The importers follow the documented
  APIs and have saved-response tests; they have not yet been run live with a key.
- **Demo market**: when markets are closed or the feed has stalled, the terminal offers
  eighteen built-in simulated scenarios in SPX, SPY and QQQ options, with drill objectives,
  gaps, volatility changes, overnight sessions and runs that span several sessions and days. Each run chooses a fresh seed, or
  repeats one you supply. Add your own JSON files with `--scenario-dir`; generated
  prices stay labelled simulated. `--provider demo` rotates regular scenarios as
  the server's own feed without network services. [Scenario format](scenarios.md).
- **Alerts**: price levels on an underlying (drawn on its chart) and every fill, shown in
  the terminal and as browser notifications with an optional chime while it is open;
  assignments, exercises at expiry and dividends are always announced. Floor room,
  daily loss, guardrails, pending limits and a nearby profit target also raise alerts,
  once per threshold per account and trading day.
- **External notifications**: send fills, rejected orders, floor warnings, rule trips,
  assignments, exercises, ready playbooks and stalled feeds to Discord, Telegram,
  ntfy or a webhook. Each channel has filters and a test button in Alerts settings.
  Delivery runs separately from trading; replays and drills never send.

## Operations

- **Status**: feed health per underlying, trading sessions, queue and analytics timing,
  and the local volatility history store.
- **Scripting and agents**: a checked OpenAPI contract, a standard-library Python
  client and an MCP server, with scoped tokens and journal actor attribution.
- **Broker data**: Tradier snapshot polling and tastytrade DXLink streams, for
  traders whose brokerage account includes real-time option data; credentials stay
  in the environment and neither adapter can place orders.
- **Install**: Homebrew on Apple Silicon, Docker Compose, release archives or source.
  Optional browser update notices are off by default.
- Light and dark themes, keyboard shortcuts (number keys follow the sidebar: Brief
  is 7 with a paper account, 2 on analytics-only servers; arrows step expiries on Trade).

## Paper trading

The engine simulates orders on European cash-settled index options (SPX, XSP, NDX,
RUT and their weeklies) and American equity and ETF options (SPY, QQQ, single stocks)
against displayed quotes: market and marketable orders take the far side up to the
displayed size by default, with optional per-plan slippage of 0–10 ticks. Single-leg limits cap
the fill price; multi-leg orders wait if the slipped net exceeds their limit.
Rules offers **As displayed** (the existing defaults) or **Conservative** for a new
account attempt: 1 second of market-time latency, 1 slippage tick and 1 extra tick
for each additional displayed-size block. Custom rules set latency up to 60 seconds
and impact up to 10 ticks per block. With impact, limits wait when the full price
or net exceeds them. This is simulated depth. Neither model knows queue position,
hidden liquidity, or whether the market would have traded at all; a delayed feed
still gives hindsight. Resting limits fill when a later quote crosses them, and every fill
pays a per-contract fee. Positions are marked at the mid, and risk limits on dollar
delta, vega, order size, price bands and daily loss are checked before and at every
fill. A book the market has pushed over its delta or vega limit can still be closed or
hedged: only orders that would add to the excess are refused. Every product trades in its regular session (09:30 to 16:15 ET for index
options), and SPX, XSP, VIX and RUT options also trade in Cboe's overnight session
(20:15 to 09:25 ET) and the 16:15 to 17:00 curb, with limit orders only. The
market-wide circuit breakers halt trading when the S&P 500 falls 7%, 13% or 20%, with
a banner saying when trading resumes, and Cboe's published holiday schedule is read daily, so a closure it announces applies at
once. American equity
and ETF options deliver shares when exercised early or held into expiry a cent in the
money. A short one that trades below its exercise value at the close, or a call worth
less over it than a dividend going ex, can be assigned overnight, in part and at
random as real assignments are; the shares are marked, risked and closed at the
underlying's price.
Dividends are paid on them from a file you give the server (`--dividends FILE`), or
from Massive's API with a key from any of its stocks plans (`--dividends massive`),
whichever provider supplies the quotes.

Buying power follows each order's real margin: a naked short holds the usual
20%-of-spot requirement, while spreads, condors, butterflies, calendars and diagonals
hold only what they can lose, a covered call nothing beyond its shares, a short put
against short shares its value, a long call protecting short shares its strike, and a
short straddle or strangle its greater side plus the other side's value, as Reg T sets
it. An order that would use buying power must fit within it;
anything that frees buying power (closing, buying back a short, buying protection) is
always allowed, even when the account is short of it. A close whose longs also cover
other shorts must carry those shorts, like selling a spread's long leg alone.
The Positions page breaks the requirement down by underlying, naming the positions
each part holds (a spread, a covered call, a straddle, a naked short) and what it
needs, or the portfolio scan's worst point.
Custom plans can instead select portfolio margin, as Cboe's and FINRA's rules set it:
each underlying holds its largest loss across a price scan (−8% to +6% for index
products, ±15% for stocks and ETFs), at least $37.50 a contract, and buying power is
equity less that, so long options and shares count as collateral. Presets use strategy
margin and As displayed fills unless Conservative is selected; the
[paper-trading guide](paper-trading.md#account-rules-and-evaluations) has the details.

Plans set an account's rules: `practice` (the default: buying power only),
`intraday-25k|50k|100k` (buy-only, 10% target, 5% drawdown trailing every new high) or
`eod-25k|50k|100k` (any strategy, 12% target, 6% drawdown trailing each close). Touching
the floor fails the attempt and closes every position; reaching the target passes it.
On the default 15-minute delayed feed a pass is practice, not proof: any real-time chart
shows where the market went next.
`--plan` picks the main account's first plan; start a new attempt on any plan from the
Dashboard or Rules page, and add accounts from the account switcher in the sidebar.

Ordinary paper accounts survive restarts through an append-only, hash-chained journal:
the main account at `~/.openport/paper-journal.jsonl` (`--paper-journal`), the others in an
`accounts` directory beside it. Each record reaches the disk before its transaction is
published, and carries what the transaction changed, with the whole state every
thousand records; `openportd --compact-journals` rewrites journals
from older builds that way, keeping each original as `.bak`, and `--repair-journals`
cuts off a last line a full disk tore. `--no-paper` turns trading off. The engine also
models the funded phase that follows a pass (`funded-*` plans with a locking floor and
payouts). This is a simulator that funds no one, so the web terminal hides those plans
and the Payouts page; set `showFundedAccounts` in `web/src/lib/features.ts` to offer
them. [Paper trading](paper-trading.md) documents every rule, the HTTP contract and
the simulation's limits.

## Roadmap

- [x] A flatten that works until flat: spreads close as one order, large closes split,
      remainders work on later quotes and exits stay until flat; buying back shorts quoted
      only on the ask, abandoning worthless longs and do-not-exercise instructions
- [x] Headless playbook batch backtests, independent days and carried-account evaluation attempts, API and terminal reports
- [x] Homebrew formula, Docker Compose and opt-in browser update notices
- [x] External notifications through Discord, Telegram, ntfy and generic webhooks
- [x] Versioned playbooks, staged orders, replay auto mode, adherence and historical pass-odds estimates
- [x] Private visitor sandbox accounts on public simulated demos, with idle expiry and rate limits
- [x] Checked OpenAPI contract, Python client and MCP tools, scoped tokens and actors

- [x] Plan-locked limits, personal guardrails, order previews and size to floor, breach
      estimates, intraday equity history and rule alerts
- [x] Pricing core: Black-76 and Black-Scholes-Merton with full Greeks, safeguarded IV
      solver, Cox-Ross-Rubinstein and Leisen-Reimer trees
- [x] Providers: Cboe, Databento, Massive, ThetaData, Tradier and tastytrade, with
      record and replay of any feed; broker adapters await live-account validation
- [x] Local Databento and ThetaData historical-day imports; live-key validation pending
- [x] Chain liquidity: session volume, spread percentages and ticket warnings
- [x] Chain analytics: parity forwards, IV and Greeks, SVI surfaces, GEX and VEX, and
      de-Americanised IV for equity options
- [x] Paper trading against live quotes: risk limits, scenarios, index and American
      equity and ETF options
- [x] Evaluation simulator: profit targets, trailing drawdowns, resets and a trade
      journal, with the funded phase and payouts in the engine
- [x] Strategies: multi-leg orders with spread-aware buying power, held strategies as
      positions, rolls, risk graph, probability of profit with skew, and templates
- [x] Terminal: underlying chart, order changes in place, flatten, multiple named
      accounts and trading recorded days in replay
- [x] Paper trading in Cboe's overnight and curb sessions
- [x] GTC limit orders, order notes and tags, spread brackets and held-spread exits
- [x] EXTO/GTC_EXTO protection in all product sessions, timestamp GTD, and limit flatten in GTH/curb
- [x] Roll the put or call side of a four-leg strategy
- [x] Trade notes and tags, with reports by tag, and price and fill alerts
- [x] Fill context, trade and strategy excursions, CSV export and day plans and reviews
- [x] P&L attribution by delta, gamma, vega and theta
- [x] Stock positions from early exercise and from exercise and assignment at expiry
- [x] Expiry hours as the exchanges run them: ETF options to 16:15, auto-close five
      minutes before each contract's last trade, with the chain staying on 0DTE until then
- [x] Demo market: simulated days to trade in Replay or rotate as an offline server feed
- [x] Scenario library: simulated drills with fresh or repeatable seeds, start times and
      replay journals that are kept
- [x] Deterministic replay batches, verifiable run journals and synchronous market-time stepping
- [x] Multi-day and cross-session scenario runs: regular, curb and overnight sessions over
      several days on one account, steppable to a date and time, with journals that verify
- [x] Shares in the journal, and early assignment of shorts trading below exercise value
- [x] Partial, random early assignment, and dividend risk on short calls
- [x] Market-wide circuit breakers, with a banner and kept across restarts, and Cboe's
      holiday schedule read daily
- [x] Dividends from Massive's API
- [x] PM settlement on the provider's official close, revisions included
- [x] Cboe's delayed feed from its quote pages when its data files fall behind
- [x] Dividends from a file you supply
- [x] Optional market-time fill latency and simulated size impact, with As displayed and Conservative presets
- [x] Optional slippage, and portfolio margin as Cboe's and FINRA's rules set it
- [x] Known cash dividends in the American exercise model
- [x] IWM and DIA by default, with stocks and ETFs marked at their regular close
      outside the session
- [x] Cboe's delayed data from its new host, following redirects if it moves again
- [x] Current model-free IV, ATM/skew, realized volatility, cones and implied session moves
- [x] Local volatility history, IV rank and percentile, labelled index proxies and ex-post VRP
- [x] Morning brief with market levels, account allowance, day notes and chart overlays
- [x] Stop-limit exits on single contracts and spreads, banded around their stop, and
      exits that re-arm or shrink so a partly filled position stays protected
- [x] Resumable interrupted replay runs, archived runs' working orders ended with the
      run, and demo restarts that resume the rest of their date with a breaker reference
- [x] Exports that name their replay run and attempt, trades that say what closed them,
      holiday-aware weekday reports and one win-rate definition
- [x] Order history: end times, changes and refusals, what a working order waits for,
      and the numbers behind every rejection or risk cancel
- [x] Fuller previews: fill schedules, order changes, flatten dry runs and sizing to
      buying power apart from the floor; held-book risk warnings; what-if comparison of
      candidate adjustments
- [x] Fill audit trail: the bid, ask, sizes, observation and first quote time on every fill
- [x] Executable-quote flags and the displayed size an account has left, in the chain, tickets and previews
- [x] P&L by Greek for each round trip and for contracts closed today, to the micro-dollar
      and flagged where it falls back; planned risk for combo stops and covered calendars;
      positions with their round trip and the contract's lifetime this attempt
- [x] Whole trades across rolls and adjustments, with their own review, and legs
      entered one by one grouped into one trade; held strategies over the API
- [x] Return on buying power for trades, strategies and whole trades, in the Journal,
      playbook reports and backtests
- [x] Same-expiry strike rolls and whole-condor rolls, exits resized in place and DAY or
      GTC changed on a resting order
- [x] Kill latch history, when a reset can clear it, and guardrail refusals with their numbers
- [x] Client order IDs scoped to an attempt and looked up by ID, conflicting reuses
      recorded nowhere, shorts closed first on auto-close, and a buy-only manual close
      that supersedes its own armed stop
- [x] Preview warnings for stops, targets and triggers already reached, a stop given as a
      limit price, and slippage that pushes a market order outside the price band;
      IOC remainders that say whether the limit was short or the book was used up
- [x] Marketable buy limits reserve their expected fill and armed buy stops their level;
      dividends paid in whole cents; an idle account's views as of the feed's time;
      working-order ladders that no longer slow every command
