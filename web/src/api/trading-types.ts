import type { Num } from "./types"

/** Accounting values stay decimal strings, including request bodies. */
export type Money = string
/** open: writes need no token (one sent is still checked); token: writes need one; disabled: none can write. */
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
  /** Every short option needs a long of its type expiring with it or later; absent from older servers. */
  defined_risk?: boolean
  /** Older servers use zero slippage and strategy margin. */
  slippage_ticks?: number
  fill_latency_ms?: number
  impact_ticks?: number
  margin?: "strategy" | "portfolio"
  buying_power: boolean
  expiry_cutoff_seconds: number
  payouts: PayoutRules | null
}
/**
 * A reason code with its numeric evidence: `actual` against `limit` for a numeric
 * check, and `scope`, the underlying or "aggregate" it applies to. The evidence is
 * absent on older servers and null when a check has none.
 */
export interface Decision { code: string; message: string; actual?: number | null; limit?: number | null; scope?: string | null }
/** `requirement` is `short_requirement` under a name that fits portfolio margin too; absent on older servers. */
export interface BuyingPower { available: Money; reserved: Money; short_requirement: Money; requirement?: Money }
/**
 * P&L explained by the Greeks, in dollars to the micro-dollar (older servers: to the cent): each stretch a
 * position is held at one size, split by its Greeks at the start. `other` is what they leave unexplained;
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
  /** Some of it is `other` because a stretch moved without valuations at both ends; absent from older servers. */
  fallback?: boolean
}
/** Every round trip in a holding this attempt, the open one included. */
export interface Lifetime { round_trips: number; realised: Money; fees: Money; net: Money }
/** A contract or shares traded today and no longer held, with the day's P&L by Greek. */
export interface ClosedAttribution { symbol: string; kind: "option" | "shares"; underlying: string; attribution: Attribution }
export interface EvaluationDay {
  low_equity?: Money | null
  high_equity?: Money | null
  low_at?: string | null
  high_at?: string | null
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
  blocked: Decision | null
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
  day_low_equity?: Money | null
  day_high_equity?: Money | null
  day_low_at?: string | null
  day_high_at?: string | null
  closest_floor?: Money | null
  closest_floor_at?: string | null
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
/**
 * Something about the held book worth acting on. `actual` and `limit` depend on the
 * code (docs/paper-trading.md, Risk warnings).
 */
export interface RiskWarning {
  code: "DELTA_LIMIT" | "VEGA_LIMIT" | "DELTA_HEADROOM" | "SOFT_FLOOR" | "SOFT_FLOOR_ROLLOVER" | "FLOOR_RATCHET" | "EXPIRY_DELIVERY" | "EARLY_ASSIGNMENT" | "EX_DIVIDEND"
  severity: "warning" | "info"
  /** The underlying, or "aggregate" for the account. */
  scope: string
  symbol: string | null
  message: string
  actual: number | null
  limit: number | null
}
export interface Account {
  breach?: Breach
  /** Absent on older servers. */
  warnings?: RiskWarning[]
  guardrails?: Guardrails
  guardrail_state?: GuardrailState
  account_version: string
  time: string
  rules: AccountRules
  evaluation: Evaluation
  buying_power: BuyingPower
  /** Null outside the funded phase. */
  payout: PayoutStatus | null
  attempts: AttemptSummary[]
}
export interface FillContext {
  spot: Num
  spot_source: string | null
  iv: Num
  delta: Num
  years: Num
  equity: Money | null
  floor_room: Money | null
  buying_power: Money | null
}
export interface Excursion { pnl: Money; time: string; spot: Num }
export interface TradeReview {
  mae: Money | null
  mfe: Money | null
  worst: Excursion | null
  best: Excursion | null
  planned_risk: Money | null
  give_back: Money | null
  heat: Num
  r_multiple: Num
}
export interface DayNote { plan: string; review: string; time?: string | null }
export interface Trade {
  id: string
  attempt: number
  entry_context?: FillContext | null
  exit_context?: FillContext | null
  review?: TradeReview
  strategy_id?: string | null
  strategy_review?: TradeReview | null
  /** The whole trade it is in, by its first round trip's ID (its own unless rolled, adjusted or grouped); absent from older servers. */
  group?: string
  /** The buying power its entry needed on its own, and net over it once closed; absent from older servers. */
  buying_power?: Money
  return_on_buying_power?: Num
  /** The same for its strategy's legs together; null outside a strategy. */
  strategy_buying_power?: Money | null
  strategy_return_on_buying_power?: Num
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
  /** The engine's trading date of the close, market holidays known; null while open, absent from older servers. */
  trading_day?: string | null
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
  closure: "settlement" | "reset" | "exercise" | "assignment" | "abandon" | null
  /** What closed it: the closure, or its last reducing fill's order. Null while open; absent from older servers. */
  closed_by?: TradeExit | null
  /** Why the reducer liquidated it when closed_by is "system": "target", "drawdown", "soft_floor" or "expiry". */
  system_reason?: string | null
  /** Its P&L by Greek over its life; null for one open from before they were kept, absent from older servers. */
  attribution?: Attribution | null
  fills: string[]
  /** The trader's note ("" for none) and tags; absent from older servers. */
  note?: string
  tags?: string[]
}
/** How a round trip ended: the trader's own order, a bracket exit, a flatten, a playbook's close, a reducer liquidation, or a closure. */
export type TradeExit = "order" | "stop_loss" | "take_profit" | "flatten" | "playbook" | "system" | "settlement" | "exercise" | "assignment" | "abandon" | "reset"
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
  trading_day?: string | null
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
  /** Its P&L by Greek over its life (all delta, dividends in other); absent from older servers. */
  attribution?: Attribution | null
  /** Stock fill IDs, "s"-prefixed ("s3"); older servers sent bare numbers. */
  fills: string[]
  /** The trader's note ("" for none) and tags, as on option trades; absent from older servers. */
  note?: string
  tags?: string[]
}
/** One change in shares held, and how it came about. */
export interface StockFill { id: string; symbol: string; shares: number; price: Money; time: string; source: ShareSource; option: string | null }
/** A dividend paid on (negative: charged to short) shares held into its ex-date. */
export interface DividendPaid { symbol: string; ex_date: string; per_share: Money; shares: number; amount: Money; time: string }
/** A trade with more than one entry: a roll, an adjustment or round trips the account grouped. */
export interface WholeTrade {
  id: string
  attempt: number
  underlying: string
  status: "open" | "closed"
  opened: string
  closed: string | null
  trading_day: string | null
  /** Its round trips' trade IDs, in the order they opened. */
  round_trips: string[]
  /** The orders that opened them. */
  entries: number
  gross: Money
  fees: Money
  net: Money
  /** The open round trips' unrealized P&L; null when closed or a mark is missing. */
  unrealised: Money | null
  /** The most buying power its round trips needed at once, and net over it once closed; absent from older servers. */
  buying_power?: Money
  return_on_buying_power?: Num
  /** Its whole-trade review: from the first entry, or from `review_since` when the account grouped it later. */
  review: TradeReview | null
  review_since: string | null
}
export interface TradesResponse {
  account_version: string
  attempt: number
  trades: Trade[]
  /** Whole trades with more than one entry, newest first; absent from older servers. */
  groups?: WholeTrade[]
  share_trades?: ShareTrade[]
  day_notes?: Record<string, DayNote>
  /** Every change in shares and every dividend, oldest first; absent from older servers. */
  stock_fills?: StockFill[]
  dividends?: DividendPaid[]
  /** The replay run these trades come from; null for a live account, absent from older servers. */
  run?: RunIdentity | null
}
/** A replay run: its id and what it replayed (a scenario and seed, or a recording). */
export interface RunIdentity { id: string | null; scenario: string | null; seed: string | null; recording: string | null; date: string | null }
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
export type FillModel = "as_displayed" | "conservative"
export type ResetRequest = { reason: string; fill_model?: FillModel } & ({ plan: string } | { initial_cash: Money; rules: AccountRules })
export type Side = "buy" | "sell"
/** Option triggers compare the order's executable side; underlying ones compare spot. */
export interface Trigger { source: "option" | "underlying" | "combo"; direction: "at_or_below" | "at_or_above"; level: Money }
/** A trigger (a stop: market when reached), a limit (a resting take-profit), or both (a stop-limit: a GTC limit once reached). */
export type ExitSpec = { trigger: Trigger; limit_price?: Money } | { limit_price: Money; trigger?: Trigger }
export interface Bracket { stop_loss?: ExitSpec; take_profit?: ExitSpec }
/** One leg of a multi-leg order: `ratio` contracts per unit. */
export interface OrderLeg { symbol: string; side: Side; ratio: number }
type Pricing = { type: "limit"; limit_price: Money; time_in_force: "day" | "gtc" | "ioc" }
  | { type: "market"; time_in_force: "ioc"; limit_price?: never }
export type NewOrder = { tags?: string[]; note?: string } & ({
  client_order_id: string
  symbol: string
  side: Side
  quantity: number
  trigger?: Trigger
  bracket?: Bracket
  /** A whole trade to join, by one of its open round trips' IDs (an adjustment). */
  group?: string
  legs?: never
} | {
  client_order_id: string
  /** Two to four legs on one underlying, filled together. */
  legs: OrderLeg[]
  trigger?: Trigger
  bracket?: Bracket
  exits_only?: boolean
  /** A whole trade to join, by one of its open round trips' IDs (an adjustment). */
  group?: string
  /** Units of the strategy. */
  quantity: number
  symbol?: never
  side?: never
}) & Pricing
/** What keeps an open order from filling now. */
export interface OrderWait {
  code: "TRIGGER" | "REGULAR_SESSION" | "INVALID_QUOTE" | "STALE_QUOTE" | "FILL_LATENCY" | "NEWER_QUOTE" | "LIMIT" | "DISPLAYED_SIZE" | "STALE_DATA"
  message: string
}
/** A change asked of a resting order: the terms requested (null where kept), those before it, and its outcome. */
export interface OrderChangeRecord {
  time: string
  actor: string
  quantity: number | null
  limit_price: Money | null
  trigger_level: Money | null
  previous: { quantity: number; limit_price: Money | null; trigger_level: Money | null }
  applied: boolean
  reason: Decision | null
}
export interface Order {
  actor?: string
  tags?: string[]
  note?: string
  exits_only?: boolean
  /** The whole trade it joins; null for none, absent from older servers. */
  group?: string | null
  id: string
  client_order_id: string
  /** Null for a multi-leg order; see `legs`. */
  symbol: string | null
  underlying: string
  side: Side | null
  /** A multi-leg order's legs; its prices are net per unit, negative for a credit. */
  legs?: OrderLeg[] | null
  type: "limit" | "market"
  time_in_force: "day" | "gtc" | "ioc"
  quantity: number
  filled_quantity: number
  remaining_quantity: number
  limit_price: Money | null
  average_fill_price: Money | null
  status: "working" | "partially_filled" | "filled" | "cancelled" | "rejected" | "armed"
  reason: Decision | null
  accepted_at: string
  day_end: string | null
  /** When it stopped working; null while open. Absent on older servers, as are the changes. */
  ended_at?: string | null
  /** An open order: what keeps it from filling now, or null. */
  waiting?: OrderWait | null
  modified_at?: string | null
  changes?: OrderChangeRecord[]
  /** System orders liquidate or auto-close; absent on older servers. */
  origin?: "user" | "system"
  /**
   * A close the account works for the trader, as a flatten's are: it only reduces, shrinks
   * with its position and works on later quotes until it fills or its session ends.
   */
  reduce_only?: boolean
  trigger?: Trigger | null
  triggered_at?: string | null
  bracket?: { stop_loss: { trigger: Trigger | null; limit_price: Money | null } | null; take_profit: { trigger: Trigger | null; limit_price: Money | null } | null } | null
  role?: "stop_loss" | "take_profit" | null
  parent?: string | null
  oco?: string | null
  stop_loss_order?: string | null
  take_profit_order?: string | null
}
/** The book a fill traded against: size_left is the taken side's displayed size still free for paper orders before it, quoted_at when the quote was first given. */
export interface FillQuote {
  observation: string
  bid: Money | null
  ask: Money | null
  bid_size: number
  ask_size: number
  size_left: number
  quoted_at: string
  age_seconds: number
}
export interface Fill {
  actor?: string
  context?: FillContext | null
  /** Null for fills recorded before the book was kept; absent on older servers. */
  quote?: FillQuote | null
  id: string
  order_id: string
  /** The attempt it belongs to; absent from older servers. */
  attempt?: number
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
  /** While awaiting settlement: on the recorded closing print, or by a value entered by hand (AM series, or no print arrived). */
  settle_by?: "closing_print" | "manual" | null
  /** It cannot be sold now (its quote shows only an ask, or it has expired), so a long can be abandoned; absent on older servers. */
  no_bid?: boolean
  /** A long option instructed not to be exercised at expiry; absent on older servers. */
  do_not_exercise?: boolean
  greeks: { delta: Num; gamma: Num; vega: Num; theta: Num; dollar_delta: Num; dollar_gamma_1pct: Num; vega_dollars: Num; theta_dollars: Num }
  /** Today's P&L by Greek for this contract; null until its first fill or rollover on this server. */
  attribution?: Attribution | null
  /** The round trip in progress (its realised and fees are this row's) and every round trip in the contract this attempt; absent from older servers. */
  trade?: string | null
  lifetime?: Lifetime | null
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
  trade?: string | null
  lifetime?: Lifetime | null
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
  /** The requirement by underlying with what holds it; absent from older servers. */
  margin?: MarginUnderlying[]
  /** Today's P&L by Greek; absent from older servers. */
  attribution?: Attribution
  /** Current quotes whose displayed size this account's orders have taken some of; absent from older servers. */
  liquidity_used?: LiquidityUsed[]
  /** Contracts and shares traded today and no longer held; absent from older servers. */
  closed?: ClosedAttribution[]
  /** Held positions by the whole trade each is in; absent from older servers. */
  strategies?: HeldStrategy[]
}
/** An OSI symbol, or the underlying for shares, with the signed contracts or shares a part takes. */
export interface MarginLeg { symbol: string; quantity: number }
/** Positions that hold part of an underlying's strategy-margin requirement together. */
export interface MarginPart {
  kind: "naked" | "vertical" | "covered" | "straddle" | "short_shares" | "protected_shares" | "worst_loss" | "long"
  legs: MarginLeg[]
  requirement: Money
}
/** Where a portfolio-margin scan loses most, and the contract minimum under it. */
export interface MarginScan { loss: Money; spot_percent: number; vol_points: number; minimum: Money }
/** Strategy margin's parts add up to the requirement; portfolio margin's scan sets it. */
export interface MarginUnderlying { underlying: string; requirement: Money; parts: MarginPart[]; scan: MarginScan | null }
export interface LiquidityUsed { symbol: string; bid_size: number; ask_size: number; bid_left: number; ask_left: number }
/** A whole trade still held: its open legs, and every round trip of it in its realized P&L and fees. */
export interface HeldStrategy {
  id: string
  underlying: string
  opened: string
  legs: { symbol: string; quantity: number; trade: string }[]
  round_trips: number
  entries: number
  realised: Money
  fees: Money
  unrealised: Money | null
  net: Money | null
}
/** The trade each named round trip is in after grouping or ungrouping. */
export interface GroupResponse { account_version: string; groups: Record<string, string> }
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
  time?: string
  breach?: Breach
  /** Absent on older servers. */
  warnings?: RiskWarning[]
  guardrails?: Guardrails
  guardrail_state?: GuardrailState
  pending_limits?: Limits | null
  pending_guardrails?: Guardrails | null
  pending_applied_day?: string | null
  pending_applied_at?: string | null
  pending_effective?: "next_trading_day" | null
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
export type CreateAccountRequest = { name: string; fill_model?: FillModel } & ({ plan: string } | { initial_cash: Money; rules: AccountRules })
export interface CreateAccountResponse { account: { id: string; name: string; account_version: string; plan: string | null; equity: Money } }
/** Delivered shares a flatten could not close, as held after it, and why. */
export interface KeptStock { symbol: string; shares: number; reason: Decision }
/** A position a flatten left open: contracts still held, those still being worked, and why the rest are not. */
export interface FlattenResidual {
  symbol: string
  underlying: string
  /** Signed contracts still held. */
  quantity: number
  /** Contracts its reduce-only closes are still working on later quotes. */
  working: number
  reason: Decision | null
}
/**
 * Each closing order with its outcome; a rejected one carries its reason. The
 * share fields are absent from older servers.
 */
export interface ClosePositionsResponse extends CancelAllResponse {
  orders: Order[]
  fills: Fill[]
  /** Shares it closed. */
  stock_fills?: StockFill[]
  /** Shares it could not close. */
  kept_stocks?: KeptStock[]
  /** Positions still open after it; absent on older servers. */
  residuals?: FlattenResidual[]
}
export interface KillResponse { account_version: string; kill: KillState; cancelled_orders: string[] }
export interface SettlementResponse { account_version: string; position_closed: boolean }

export interface Guardrails {
  soft_floor: Money
  soft_floor_percent: number
  max_opening_trades: number
  cooldown_loss: Money
  cooldown_minutes: number
  profit_lock: Money
}
export interface GuardrailState {
  opening_trades: number
  latched: string[]
  cooldown_until: string | null
  cooldown_seconds: number
  soft_floor: Money | null
}
export interface BreachLevel { points: number; percent: number; touch_probability: number | null }
export interface Breach {
  room: Money | null
  soft_room: Money | null
  complete: boolean
  model: string
  underlyings: {
    underlying: string; spot: number; complete: boolean; close_sigma: number | null; down: BreachLevel | null; up: BreachLevel | null
    /** Levels at the personal soft floor; the same as down/up without a plan floor. Absent on older servers. */
    soft_down?: BreachLevel | null; soft_up?: BreachLevel | null
  }[]
}
/** A block of contracts at one price, as a fill would trade it. */
export interface PreviewFill { symbol: string; side: Side; quantity: number; price: Money }
/** What submitting the order now would execute (a private dry run), and its full size block by block. */
export interface PreviewExecution {
  status: Order["status"]
  filled_quantity: number
  remaining_quantity: number
  reason: { code: string; message: string } | null
  fills: PreviewFill[]
  average_fill_price: Money | null
  schedule: PreviewFill[]
  average_price: Money | null
}
export interface OrderPreview {
  account_version: string
  decision: string
  reason: Decision | null
  /** working: available while the order works, before it fills; absent on older servers. */
  buying_power: { required: Money; before: Money; working?: Money | null; after: Money | null }
  exposure_change: { dollar_delta: number; dollar_gamma_1pct: number; vega: number; theta: number } | null
  max_loss: Money | null
  max_loss_basis: "expiry_payoff" | "scenario_grid" | null
  equity_at_max_loss: Money | null
  breaches_floor: boolean | null
  breaches_soft_floor: boolean | null
  /** Units that fit buying power, the limits and the floor share; null when sizing is unavailable. */
  max_units: number | null
  /** Units that fit buying power and the limits, floor room aside. Absent on older servers. */
  max_units_buying_power?: number | null
  /** Units whose loss fits the floor share; null without a plan or soft floor. Absent on older servers. */
  max_units_floor?: number | null
  breach: Breach
  /** Absent on older servers. */
  execution?: PreviewExecution
  /** Each leg's quote; absent on older servers. */
  liquidity?: PreviewLiquidity[]
  simulated: true
}
/** An account as POST /api/orders/what-if projects it, measured from today's equity. */
export interface WhatIfAccount {
  equity: Money
  buying_power: Money
  exposure: { dollar_delta: number | null; dollar_gamma_1pct: number | null; vega: number | null; theta: number | null } | null
  /** The worst loss on the spot × volatility grid from today's equity, trading costs included. */
  max_loss: Money | null
  equity_at_max_loss: Money | null
  breaches_floor: boolean | null
  breaches_soft_floor: boolean | null
  /** Each cell's P&L from today's equity, spot-major. */
  scenarios: { spot_percent: number[]; vol_points: number[]; pnl: (number | null)[][]; complete: boolean }
  breach: Breach
}
export interface WhatIfCandidate {
  name: string
  decision: string
  reason: Decision | null
  orders: { decision: string; reason: Decision | null }[]
  /** Null when an order could not be projected (no contract or quote). */
  after: WhatIfAccount | null
}
/** POST /api/positions/close/preview: a flatten's dry run on a private copy of the account. */
export interface FlattenPreview {
  account_version: string
  decision: string
  reason: Decision | null
  cancelled_orders: string[]
  /** Its closing orders as the current quotes would leave them, without IDs. */
  orders: { symbol: string; underlying: string; side: Side; quantity: number; filled_quantity: number; average_fill_price: Money | null
    status: Order["status"]; reason: Decision | null }[]
  fills: { symbol: string; side: Side; quantity: number; price: Money; fee: Money }[]
  stock_fills: { symbol: string; shares: number; price: Money }[]
  kept_stocks: KeptStock[]
  /** What would still be held in scope, signed. */
  remaining: { symbol: string; underlying: string; quantity: number }[]
  remaining_shares: { symbol: string; shares: number }[]
  current: WhatIfAccount | null
  after: WhatIfAccount | null
  simulated: true
}
export interface WhatIfResponse { account_version: string; current: WhatIfAccount; candidates: WhatIfCandidate[]; simulated: true }
/** The quote a previewed leg takes: size_left is what this account's orders have left of its displayed size. */
export interface PreviewLiquidity {
  symbol: string
  side: Side
  contracts: number
  executable: boolean
  reason: { code: string; message: string } | null
  displayed: number
  size_left: number
}
export interface EquitySample {
  time: string
  day: string
  attempt: number
  equity: Money
  floor: Money | null
  peak: Money
  target: Money | null
  tomorrow_floor: Money | null
  fill: string | null
}
export interface EquityHistory { samples: EquitySample[]; error: string | null }
