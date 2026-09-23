import type { Num } from "./types"

/** Accounting values stay decimal strings, including request bodies. */
export type Money = string
export type WriteMode = "open" | "token" | "disabled"
export type EvaluationStatus = "active" | "passed" | "failed"
export interface TradingStatus {
  enabled: boolean
  reason: string | null
  account_version: string
  kill_latched: boolean
  write: WriteMode
  fee_per_contract?: Money
  initial_cash?: Money
  /** Active rules' display name; absent on older servers, null without a plan. */
  plan?: string | null
  /** Null without a target or drawdown rule. */
  evaluation?: EvaluationStatus | null
}
export interface AccountRules {
  plan: string | null
  profit_target: Money | null
  max_drawdown: Money | null
  drawdown_mode: "intraday" | "end_of_day"
  buy_only: boolean
  buying_power: boolean
  expiry_cutoff_seconds: number
}
export interface BuyingPower { available: Money; reserved: Money; short_requirement: Money }
export interface EvaluationDay { day: string; open_equity: Money; close_equity: Money; peak: Money; floor: Money | null }
export interface Evaluation {
  enabled: boolean
  attempt: number
  status: EvaluationStatus
  started: string
  starting_balance: Money
  equity: Money
  /** Every position has a mark; rules only decide on fully marked equity. */
  marked: boolean
  valuation_complete: boolean
  profit: Money
  peak: Money
  floor: Money | null
  drawdown_buffer: Money | null
  target_equity: Money | null
  target_remaining: Money | null
  decided_at: string | null
  decided_equity: Money | null
  decision: string | null
  day: string
  day_open_equity: Money
  day_close_equity: Money
  days: EvaluationDay[]
}
export interface AttemptSummary {
  attempt: number
  plan: string | null
  started: string
  ended: string
  starting_balance: Money
  final_equity: Money
  status: EvaluationStatus
  decision: string | null
}
export interface Account {
  account_version: string
  time: string
  rules: AccountRules
  evaluation: Evaluation
  buying_power: BuyingPower
  attempts: AttemptSummary[]
}
export interface Trade {
  id: string
  attempt: number
  symbol: string
  underlying: string
  expiry: string
  settlement: "AM" | "PM"
  strike: number
  type: "call" | "put"
  direction: "long" | "short"
  status: "open" | "closed"
  opened: string
  closed: string | null
  duration_seconds: number | null
  quantity: number
  max_quantity: number
  opened_contracts: number
  closed_contracts: number
  average_open: Money | null
  average_close: Money | null
  cost: Money
  gross: Money
  fees: Money
  net: Money
  /** Net over entry cost, closed trades only. */
  return: Num
  mark: Money | null
  unrealised: Money | null
  closure: "settlement" | "reset" | null
  fills: string[]
}
export interface TradesResponse { account_version: string; attempt: number; trades: Trade[] }
export interface Plan { id: string; name: string; summary: string; initial_cash: Money; rules: AccountRules }
export interface PlansResponse { plans: Plan[] }
export type ResetRequest = { reason: string } & ({ plan: string } | { initial_cash: Money; rules: AccountRules })
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
  /** System orders liquidate or auto-close; absent on older servers. */
  origin?: "user" | "system"
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
  buying_power?: BuyingPower
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
