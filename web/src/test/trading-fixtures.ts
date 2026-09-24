import type { Chain, Expiry, OptionQuote, Status, Summary } from "../api/types"
import type { Account, Bucket, Fill, Limits, Order, Plan, Portfolio, Risk, ShareTrade, Trade, TradingStatus } from "../api/trading-types"
import type { TicketSelection } from "../components/OrderTicket"

export const time = "2026-09-23T15:30:00Z"
export const trading: TradingStatus = { enabled: true, reason: null, account_version: "17", kill_latched: false, write: "open" }
export const status: Status = {
  trading, provider: { name: "fixture", realtime: true, delay_seconds: 0, trades: true, open_interest: true, vendor_greeks: true },
  feed: { state: "live", message: "Connected", updated: time },
  underlyings: [{ symbol: "SPX", spot: 7000, as_of: time, version: 1, expiries: 1, options: 2, has_tradable_contracts: true }],
  engine: { events: 100, events_per_second: 2, analytics_ms: 1, contracts: 2, uptime_seconds: 60 },
}
export const expiry: Expiry = {
  id: "2026-10-16PM", expiry: "2026-10-16", settlement: "PM", expiry_time: "2026-10-16T20:00:00Z", days: 23,
  forward: 7010, discount: .99, rate: .04, rate_fitted: true, atm_iv: .2, gex: 100000, vex: 1000, strikes: 1, style: "european",
}
export const quote: OptionQuote = {
  symbol: "SPXW  261016C07000000", bid: 4.5, ask: 4.6, mid: 4.55, bid_size: 10, ask_size: 3, tradable: true, untradable_reason: null,
  iv: .2, bid_iv: .19, ask_iv: .21, delta: .5, gamma: .002, vega: 12.25, theta: -.85, vanna: null, oi: 1000, vendor_iv: .2,
}
export const chain: Chain = {
  symbol: "SPX", spot: 7000, as_of: time, version: 1, expiry,
  strikes: [{ strike: 7000, iv: .2, gex: 100000, vex: 1000, call: quote, put: { ...quote, symbol: "SPXW  261016P07000000", delta: -.5 } }],
}
export const summary: Summary = {
  symbol: "SPX", spot: 7000, as_of: time, version: 1, compute_ms: 1, expiries: [expiry],
  exposure: { gex: 100000, vex: 1000, gamma_flip: null, call_wall: null, put_wall: null },
}
export const selection: TicketSelection = { symbol: quote.symbol!, underlying: "SPX", expiry, strike: 7000, optionType: "call", cell: "ask", price: "4.60" }
export const order: Order = {
  id: "order-1", client_order_id: "fixture-client-1", symbol: quote.symbol!, underlying: "SPX", side: "buy", type: "limit", time_in_force: "day",
  quantity: 5, filled_quantity: 2, remaining_quantity: 3, limit_price: "4.60", average_fill_price: "4.60", status: "partially_filled",
  reason: null, accepted_at: time, day_end: "2026-09-23T20:15:00Z",
}
export const fill: Fill = { id: "fill-1", order_id: order.id, symbol: quote.symbol!, underlying: "SPX", side: "buy", quantity: 2, price: "4.60", fee: "1.30", quote_time: time, time }
export const portfolio: Portfolio = {
  account_version: "17", time, cash: "99078.70", equity: "99988.70", start_of_day_equity: "100000.00", day_pnl: "-11.30",
  realised: "0.00", unrealised: "-10.00", fees: "1.30", valuation_complete: false, quality_flags: ["AWAITING_SETTLEMENT"],
  positions: [{
    symbol: quote.symbol!, underlying: "SPX", expiry: expiry.expiry, settlement: "PM", strike: 7000, type: "call", quantity: 2,
    average_price: "4.60", basis: "920.00", mark: "4.55", mark_age_seconds: 2, market_value: "910.00", unrealised: "-10.00", realised: "0.00", fees: "1.30",
    fresh: true, awaiting_settlement: false,
    greeks: { delta: 100, gamma: .4, vega: 2450, theta: -170, dollar_delta: 700000, dollar_gamma_1pct: 196000, vega_dollars: 2450, theta_dollars: -170 },
  }, {
    symbol: "SPX   260918P06800000", underlying: "SPX", expiry: "2026-09-18", settlement: "AM", strike: 6800, type: "put", quantity: -1,
    average_price: "1.00", basis: "-100.00", mark: null, mark_age_seconds: null, market_value: null, unrealised: null, realised: "0.00", fees: "0.65",
    fresh: false, awaiting_settlement: true,
    greeks: { delta: null, gamma: null, vega: null, theta: null, dollar_delta: null, dollar_gamma_1pct: null, vega_dollars: null, theta_dollars: null },
  }],
}
export const limits: Limits = {
  max_order_contracts: 100, price_band_absolute: "5.00", price_band_relative: .2,
  aggregate: { dollar_delta: 5000000, vega: 100000 }, per_underlying: { dollar_delta: 2000000, vega: 50000 }, max_daily_loss: "5000.00", max_quote_age_seconds: 60, max_valuation_age_seconds: 120,
}
export const bucket: Bucket = { dollar_delta: 700000, dollar_gamma_1pct: 196000, vega: 2450, theta: -170,
  reachable: { delta_low: 700000, delta_high: 1750000, vega_low: 2450, vega_high: 6125 }, limits: limits.aggregate, delta_utilisation: .35, vega_utilisation: .06125 }
export const risk: Risk = {
  account_version: "17", limits_revision: "3", limits, complete: false, daily_loss: "11.30", kill: { latched: false, reason: null },
  aggregate: bucket, underlyings: { SPX: { ...bucket, limits: limits.per_underlying, delta_utilisation: .875, vega_utilisation: .1225 } },
  scenarios: { spot_percent: [-5, -1, 0, 1, 5], vol_points: [-10, -1, 0, 1, 10],
    pnl: [[null, -900, -850, -800, -500], [-700, -500, -300, -100, 400], [-800, -100, 0, 100, 1000], [-100, 200, 500, 800, 1200], [500, 700, 1000, 1500, 2000]],
    clamped: [[true, false, false, false, false], [], [], [], []], complete: false },
}

export const account: Account = {
  account_version: "17", time,
  rules: { plan: "Intraday 100K", phase: "evaluation", profit_target: "10000.00", max_drawdown: "5000.00", drawdown_mode: "intraday",
    lock_balance: null, buy_only: true, buying_power: true, expiry_cutoff_seconds: 300, payouts: null },
  evaluation: {
    enabled: true, attempt: 2, status: "active", started: "2026-09-21T14:00:00Z", starting_balance: "100000.00", equity: "100267.50",
    marked: true, valuation_complete: true, profit: "267.50", peak: "100300.00", floor: "95300.00", drawdown_buffer: "4967.50",
    target_equity: "110000.00", target_remaining: "9732.50", decided_at: null, decided_equity: null, decision: null,
    day: "2026-09-23", day_open_equity: "100100.00", day_close_equity: "100267.50",
    days: [{ day: "2026-09-22", open_equity: "100000.00", close_equity: "100100.00", peak: "100300.00", floor: "95300.00", realised: "100.00", qualifying: false }],
    floor_locked: false, qualifying_days: 0, cycle_started: "2026-09-21T14:00:00Z", payouts: [],
  },
  buying_power: { available: "99078.70", reserved: "0.00", short_requirement: "0.00" },
  payout: null,
  attempts: [{ attempt: 1, plan: "Practice", started: "2026-09-20T14:00:00Z", ended: "2026-09-21T14:00:00Z", starting_balance: "100000.00", final_equity: "99500.00", status: "active", decision: null }],
}
const trade: Trade = {
  id: "1", attempt: 2, symbol: quote.symbol!, underlying: "SPX", expiry: expiry.expiry, settlement: "PM", strike: 7000, type: "call",
  direction: "long", status: "closed", opened: "2026-09-22T14:19:06Z", closed: "2026-09-22T14:23:55Z", duration_seconds: 289,
  quantity: 0, max_quantity: 5, opened_contracts: 5, closed_contracts: 5, average_open: "4.25", average_close: "4.80", cost: "2125.00",
  gross: "275.00", fees: "6.50", net: "268.50", return: 268.5 / 2125, mark: null, unrealised: null, closure: null, fills: ["fill-1"],
}
export const trades: Trade[] = [
  { ...trade, id: "3", type: "put", strike: 6900, status: "open", closed: null, duration_seconds: null, quantity: 2, max_quantity: 2,
    opened_contracts: 2, closed_contracts: 0, average_close: null, gross: "0.00", fees: "1.30", net: "-1.30", return: null, mark: "3.10", unrealised: "-20.00" },
  { ...trade, id: "2", opened: "2026-09-23T15:00:00Z", closed: "2026-09-23T17:30:00Z", duration_seconds: 9000, gross: "-100.00", fees: "1.30", net: "-101.30", return: -101.3 / 2125 },
  trade,
]
/** Shares from an exercise at expiry, sold the next morning, and an assignment still held. */
export const shareTrades: ShareTrade[] = [
  { kind: "shares", id: "s3", attempt: 2, symbol: "QQQ", direction: "long", status: "open", opened: "2026-09-23T20:00:00Z", closed: null,
    duration_seconds: null, shares: 200, max_shares: 200, opened_shares: 200, closed_shares: 0, average_open: "480.00", average_close: null,
    cost: "96000.00", gross: "0.00", fees: "0.00", net: "0.00", return: null, mark: "478.50", unrealised: "-300.00",
    opened_by: "assignment", option: "QQQ   260923P00480000", closed_by: null, closing_option: null, fills: ["3"] },
  { kind: "shares", id: "s1", attempt: 2, symbol: "SPY", direction: "long", status: "closed", opened: "2026-09-22T20:15:00Z",
    closed: "2026-09-23T14:00:00Z", duration_seconds: 63_900, shares: 0, max_shares: 100, opened_shares: 100, closed_shares: 100,
    average_open: "501.00", average_close: "504.20", cost: "50100.00", gross: "270.00", dividends: "50.00", fees: "0.00", net: "320.00", return: 320 / 50_100,
    mark: null, unrealised: null, opened_by: "expiry_exercise", option: "SPY   260922C00500000", closed_by: "trade", closing_option: null, fills: ["1", "2"] },
]
export const plans: Plan[] = [
  { id: "practice", name: "Practice", summary: "No target or drawdown. Buying power applies.", initial_cash: "100000.00",
    rules: { plan: "Practice", phase: "evaluation", profit_target: null, max_drawdown: null, drawdown_mode: "intraday", lock_balance: null,
      buy_only: false, buying_power: true, expiry_cutoff_seconds: 0, payouts: null }, unlocked_by: null },
  { id: "intraday-100k", name: "Intraday 100K", summary: "Buy-only single-leg options.", initial_cash: "100000.00", rules: account.rules, unlocked_by: null },
  { id: "funded-intraday-100k", name: "Funded Intraday 100K", summary: "Unlocked by passing Intraday 100K.", initial_cash: "100000.00",
    rules: { ...account.rules, plan: "Funded Intraday 100K", phase: "funded", profit_target: null, lock_balance: "100000.00",
      payouts: { qualifying_profit: "200.00", qualifying_days: 8, withdrawal_percent: 50, split_percent: 80, minimum: "1000.00",
        caps: ["2000.00", "3000.00", "4000.00", "6000.00"] } }, unlocked_by: "intraday-100k" },
]
/** A funded account after one payout, three qualifying days into its second cycle. */
export const fundedAccount: Account = {
  ...account,
  rules: plans[2]!.rules,
  evaluation: {
    ...account.evaluation, attempt: 3, starting_balance: "100000.00", equity: "106050.00", profit: "6050.00", peak: "107500.00",
    floor: "100000.00", drawdown_buffer: "6050.00", target_equity: null, target_remaining: null, floor_locked: true,
    day: "2026-09-25", day_open_equity: "106050.00", day_close_equity: "106050.00",
    days: [
      { day: "2026-09-22", open_equity: "106000.00", close_equity: "105500.00", peak: "107500.00", floor: "100000.00", realised: "1500.00", qualifying: true },
      { day: "2026-09-23", open_equity: "105500.00", close_equity: "105800.00", peak: "107500.00", floor: "100000.00", realised: "300.00", qualifying: true },
      { day: "2026-09-24", open_equity: "105800.00", close_equity: "106050.00", peak: "107500.00", floor: "100000.00", realised: "250.00", qualifying: true },
    ],
    qualifying_days: 3, cycle_started: "2026-09-22T18:00:00Z",
    payouts: [{ number: 1, time: "2026-09-22T18:00:00Z", day: "2026-09-22", amount: "2000.00", trader_share: "1600.00", balance: "107500.00" }],
  },
  payout: {
    eligible: false, blocked: { code: "PAYOUT_NOT_ELIGIBLE", message: "Not enough qualifying days in this payout cycle", actual: 3, limit: 8 },
    number: 2, active: true, flat: true, qualifying_days: 3, required_days: 8, qualifying_profit: "200.00",
    profit: "6050.00", withdrawable: "3025.00", cap: "3000.00", maximum: "3000.00", minimum: "1000.00", trader_share: "2400.00",
    withdrawal_percent: 50, split_percent: 80,
  },
}
