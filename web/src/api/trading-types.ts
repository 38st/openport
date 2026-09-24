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
/** Funded-account withdrawals: a payout needs `qualifying_days` days of at least
 * `qualifying_profit` net realised profit since the last one. */
export interface PayoutRules {
  qualifying_profit: Money
  qualifying_days: number
  /** Share of profit one payout may take. */
  withdrawal_percent: number
  /** Trader's share of each payout. */
  split_percent: number
  minimum: Money
  /** Per payout number; the last repeats; empty is uncapped. */
  caps: Money[]
}
export interface AccountRules {
  plan: string | null
  phase: "evaluation" | "funded"
  profit_target: Money | null
  max_drawdown: Money | null
  drawdown_mode: "intraday" | "end_of_day"
  /** The trailing floor stops once it reaches this balance. */
  lock_balance: Money | null
  buy_only: boolean
  buying_power: boolean
  expiry_cutoff_seconds: number
  payouts: PayoutRules | null
}
export interface BuyingPower { available: Money; reserved: Money; short_requirement: Money }
/**
 * P&L explained by the Greeks, in dollars: each stretch a position is held at one
 * size, split by its Greeks at the start. `other` is what they leave unexplained;
 * `costs` are the spread paid against the mark at each fill, and fees.
 */
export interface Attribution {
  delta: number
  gamma: number
  vega: number
  theta: number
  other: number
  costs: number
  total: number
}
export interface EvaluationDay {
  day: string
  open_equity: Money
  close_equity: Money
  peak: Money
  floor: Money | null
  /** Net realised P&L of the day, after fees. */
  realised: Money
  /** Funded accounts: the day counted toward a payout. */
  qualifying: boolean
  /** The day's P&L by Greek; absent from older servers. */
  attribution?: Attribution
}
export interface Payout {
  number: number
  time: string
  /** Trading day in progress when requested; it and later days count toward the next payout. */
  day: string
  amount: Money
  trader_share: Money
  /** Equity when requested, before the withdrawal. */
  balance: Money
}
/** A funded account's standing for its next payout. */
export interface PayoutStatus {
  eligible: boolean
  /** The first unmet requirement. */
  blocked: { code: string; message: string; actual: number | null; limit: number | null } | null
  number: number
  active: boolean
  /** No positions and no working or armed orders. */
  flat: boolean
  qualifying_days: number
  required_days: number
  qualifying_profit: Money
  profit: Money
  withdrawable: Money
  cap: Money | null
  /** Largest amount accepted now. */
  maximum: Money
  minimum: Money
  trader_share: Money
  withdrawal_percent: number
  split_percent: number
}
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
  floor_locked: boolean
  /** Days closed since the last payout (or the start) that qualified. */
  qualifying_days: number
  cycle_started: string
  payouts: Payout[]
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
  /** Null outside the funded phase. */
  payout: PayoutStatus | null
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
  closure: "settlement" | "reset" | "exercise" | "assignment" | null
  fills: string[]
  /** The trader's note ("" for none) and tags; absent from older servers. */
  note?: string
  tags?: string[]
}
export interface TradeNote {
  note: string
  tags: string[]
}
export interface TradeNoteResponse extends TradeNote {
  account_version: string
  trade: string
}
/** How shares changed hands: an option's exercise or assignment, a trade that reduced them, the account closing them when an evaluation is decided, or a reset. */
export type ShareSource = "expiry_exercise" | "assignment" | "early_exercise" | "trade" | "rule" | "reset"
/** One round trip in an underlying's shares from exercise and assignment, flat to flat or still open. */
export interface ShareTrade {
  kind: "shares"
  id: string
  attempt: number
  symbol: string
  direction: "long" | "short"
  status: "open" | "closed"
  opened: string
  closed: string | null
  duration_seconds: number | null
  /** Signed shares held now; zero once closed. */
  shares: number
  max_shares: number
  opened_shares: number
  closed_shares: number
  average_open: Money | null
  average_close: Money | null
  cost: Money
  gross: Money
  /** Dividends received (negative: paid while short) during the round trip; in net. Absent on older servers. */
  dividends?: Money
  fees: Money
  net: Money
  return: Num
  mark: Money | null
  unrealised: Money | null
  opened_by: ShareSource
  /** The option whose exercise or assignment opened them, or null. */
  option: string | null
  closed_by: ShareSource | null
  closing_option: string | null
  fills: string[]
}
export interface TradesResponse { account_version: string; attempt: number; trades: Trade[]; share_trades?: ShareTrade[] }
export interface Plan {
  id: string
  name: string
  summary: string
  initial_cash: Money
  rules: AccountRules
  /** Funded plans: the evaluation plan ID whose pass unlocks them. */
  unlocked_by: string | null
}
export interface PlansResponse { plans: Plan[] }
export type ResetRequest = { reason: string } & ({ plan: string } | { initial_cash: Money; rules: AccountRules })
export type Side = "buy" | "sell"
/** Option triggers compare the order's executable side; underlying ones compare spot. */
export interface Trigger { source: "option" | "underlying"; direction: "at_or_below" | "at_or_above"; level: Money }
/** A stop takes a trigger (then trades at market); a take-profit takes a limit or a trigger. */
export type ExitSpec = { trigger: Trigger; limit_price?: never } | { limit_price: Money; trigger?: never }
export interface Bracket { stop_loss?: ExitSpec; take_profit?: ExitSpec }
/** One leg of a multi-leg order: `ratio` contracts per unit. */
export interface OrderLeg { symbol: string; side: Side; ratio: number }
type Pricing = { type: "limit"; limit_price: Money; time_in_force: "day" | "ioc" }
  | { type: "market"; time_in_force: "ioc"; limit_price?: never }
export type NewOrder = ({
  client_order_id: string
  symbol: string
  side: Side
  quantity: number
  trigger?: Trigger
  bracket?: Bracket
  legs?: never
} | {
  client_order_id: string
  /** Two to four legs on one underlying, filled together. */
  legs: OrderLeg[]
  /** Units of the strategy. */
  quantity: number
  symbol?: never
  side?: never
}) & Pricing
export interface Order {
  id: string
  client_order_id: string
  /** Null for a multi-leg order; see `legs`. */
  symbol: string | null
  underlying: string
  side: Side | null
  /** A multi-leg order's legs; its prices are net per unit, negative for a credit. */
  legs?: OrderLeg[] | null
  type: "limit" | "market"
  time_in_force: "day" | "ioc"
  quantity: number
  filled_quantity: number
  remaining_quantity: number
  limit_price: Money | null
  average_fill_price: Money | null
  status: "working" | "partially_filled" | "filled" | "cancelled" | "rejected" | "armed"
  reason: { code: string; message: string } | null
  accepted_at: string
  day_end: string | null
  /** System orders liquidate or auto-close; absent on older servers. */
  origin?: "user" | "system"
  trigger?: Trigger | null
  triggered_at?: string | null
  bracket?: { stop_loss: { trigger: Trigger | null; limit_price: Money | null } | null; take_profit: { trigger: Trigger | null; limit_price: Money | null } | null } | null
  role?: "stop_loss" | "take_profit" | null
  parent?: string | null
  oco?: string | null
  stop_loss_order?: string | null
  take_profit_order?: string | null
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
  /** When it expires and awaits settlement, and when it last trades (the business day before, for AM settlement); absent on older servers. */
  expiry_time?: string
  last_trade_time?: string
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
  /** Today's P&L by Greek for this contract; null until its first fill or rollover on this server. */
  attribution?: Attribution | null
}
/** Shares of an underlying, delivered by exercise and assignment. */
export interface StockHolding {
  symbol: string
  shares: number
  average_price: Money
  basis: Money
  mark: Money | null
  mark_time: string | null
  market_value: Money | null
  unrealised: Money | null
  realised: Money
  fees: Money
  fresh: boolean
  attribution: Attribution | null
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
  /** Absent from older servers. */
  stocks?: StockHolding[]
  buying_power?: BuyingPower
  /** Today's P&L by Greek; absent from older servers. */
  attribution?: Attribution
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
/** New terms for a resting order; omitted fields keep their value. */
export interface OrderChange { quantity?: number; limit_price?: Money; trigger_level?: Money }
export interface CancelAllResponse { account_version: string; cancelled_orders: string[] }
export interface AccountListItem { id: string; name: string; trading: TradingStatus; equity: Money | null }
export interface AccountsResponse { accounts: AccountListItem[] }
export type CreateAccountRequest = { name: string } & ({ plan: string } | { initial_cash: Money; rules: AccountRules })
export interface CreateAccountResponse { account: { id: string; name: string; account_version: string; plan: string | null; equity: Money } }
/** Each closing order with its outcome; a rejected one carries its reason. */
export interface ClosePositionsResponse extends CancelAllResponse { orders: Order[]; fills: Fill[] }
export interface KillResponse { account_version: string; kill: KillState; cancelled_orders: string[] }
export interface SettlementResponse { account_version: string; position_closed: boolean }
