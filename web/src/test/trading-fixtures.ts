import type { Chain, Expiry, OptionQuote, Status, Summary } from "../api/types"
import type { Bucket, Fill, Limits, Order, Portfolio, Risk, TradingStatus } from "../api/trading-types"
import type { TicketSelection } from "../components/OrderTicket"

export const time = "2026-09-23T15:30:00Z"
export const trading: TradingStatus = { enabled: true, reason: null, account_version: "17", kill_latched: false, write: "open" }
export const status: Status = {
  trading, provider: { name: "fixture", realtime: true, delay_seconds: 0, trades: true, open_interest: true, vendor_greeks: true },
  feed: { state: "live", message: "Connected", updated: time },
  underlyings: [{ symbol: "SPX", spot: 7000, as_of: time, version: 1, expiries: 1, options: 2 }],
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
export const fill: Fill = { id: "fill-1", order_id: order.id, symbol: order.symbol, underlying: "SPX", side: "buy", quantity: 2, price: "4.60", fee: "1.30", quote_time: time, time }
export const portfolio: Portfolio = {
  account_version: "17", time, cash: "99078.70", equity: "99988.70", start_of_day_equity: "100000.00", day_pnl: "-11.30",
  realised: "0.00", unrealised: "-10.00", fees: "1.30", valuation_complete: false, quality_flags: ["AWAITING_SETTLEMENT"],
  positions: [{
    symbol: order.symbol, underlying: "SPX", expiry: expiry.expiry, settlement: "PM", strike: 7000, type: "call", quantity: 2,
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
