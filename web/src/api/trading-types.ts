import type { Num } from "./types"

/** Accounting values stay decimal strings, including request bodies. */
export type Money = string
export type WriteMode = "open" | "token" | "disabled"
export interface TradingStatus {
  enabled: boolean
  reason: string | null
  account_version: string
  kill_latched: boolean
  write: WriteMode
}
export type Side = "buy" | "sell"
export type NewOrder = {
  client_order_id: string
  symbol: string
  side: Side
  quantity: number
} & ({ type: "limit"; limit_price: Money; time_in_force: "day" | "ioc" }
  | { type: "market"; time_in_force: "ioc"; limit_price?: never })
export interface Order {
  id: string
  client_order_id: string
  symbol: string
  underlying: string
  side: Side
  type: "limit" | "market"
  time_in_force: "day" | "ioc"
  quantity: number
  filled_quantity: number
  remaining_quantity: number
  limit_price: Money | null
  average_fill_price: Money | null
  status: "working" | "partially_filled" | "filled" | "cancelled" | "rejected"
  reason: { code: string; message: string } | null
  accepted_at: string
  day_end: string | null
}
export interface Fill {
  id: string
  order_id: string
  symbol: string
  underlying: string
  side: Side
  quantity: number
  price: Money
  fee: Money
  quote_time: string
  time: string
}
export interface Position {
  symbol: string
  underlying: string
  expiry: string
  settlement: "AM" | "PM"
  strike: number
  type: "call" | "put"
  quantity: number
  average_price: Money
  basis: Money
  mark: Money | null
  mark_age_seconds: Num
  market_value: Money | null
  unrealised: Money | null
  realised: Money
  fees: Money
  fresh: boolean
  awaiting_settlement: boolean
  greeks: { delta: Num; gamma: Num; vega: Num; theta: Num; dollar_delta: Num; dollar_gamma_1pct: Num; vega_dollars: Num; theta_dollars: Num }
}
export interface Portfolio {
  account_version: string
  time: string
  cash: Money
  equity: Money
  start_of_day_equity: Money
  day_pnl: Money
  realised: Money
  unrealised: Money
  fees: Money
  valuation_complete: boolean
  quality_flags: string[]
  positions: Position[]
}
export interface RiskCaps { dollar_delta: number; vega: number }
export interface Limits {
  max_order_contracts: number
  price_band_absolute: Money
  price_band_relative: number
  aggregate: RiskCaps
  per_underlying: RiskCaps
  max_daily_loss: Money
  max_quote_age_seconds: number
  max_valuation_age_seconds: number
}
export interface Bucket {
  dollar_delta: Num
  dollar_gamma_1pct: Num
  vega: Num
  theta: Num
  reachable: { delta_low: Num; delta_high: Num; vega_low: Num; vega_high: Num }
  limits: RiskCaps
  delta_utilisation: Num
  vega_utilisation: Num
}
export interface KillState { latched: boolean; reason: string | null }
export interface Scenarios {
  spot_percent: number[]
  vol_points: number[]
  pnl: Num[][]
  clamped: boolean[][]
  complete: boolean
}
export interface Risk {
  account_version: string
  limits_revision: string
  limits: Limits
  complete: boolean
  daily_loss: Money
  kill: KillState
  aggregate: Bucket
  underlyings: Record<string, Bucket>
  scenarios: Scenarios
}
export interface OrdersResponse { account_version: string; orders: Order[] }
export interface FillsResponse { account_version: string; fills: Fill[] }
export interface OrderResponse { account_version: string; order: Order }
export interface SubmitOrderResponse extends OrderResponse { fills: Fill[] }
export interface KillResponse { account_version: string; kill: KillState; cancelled_orders: string[] }
export interface SettlementResponse { account_version: string; position_closed: boolean }
