import type { Playbook, PlaybookTrade } from "./playbook-types"
import type { AccountRules, Money, PayoutRules } from "./trading-types"
/** Custom backtests use decimal money, nanosecond cutoffs and minute day boundaries. */
export type BacktestRules = Partial<Omit<AccountRules, "plan_id" | "plan" | "profit_target" | "max_drawdown" | "lock_balance" |
  "daily_loss_limit" | "profitable_day_profit" | "payouts" | "fees" | "expiry_cutoff_seconds" | "day_end">> & {
  plan?: string; profit_target?: Money; max_drawdown?: Money; lock_balance?: Money
  daily_loss_limit?: Money; profitable_day_profit?: Money; payouts?: Partial<PayoutRules>
  expiry_cutoff?: number; day_end_minutes?: number
}
export interface BacktestPlan { initial_cash: Money; fee_per_contract?: Money; rules: BacktestRules }
export type BacktestDayInput = { file: string } | { scenario: string; date?: string; seed: string }
export type BacktestStart = ({ playbook: string; playbooks?: never } | { playbooks: string[]; playbook?: never }) & { plan: string | BacktestPlan; workers?: number } & (
  { days: BacktestDayInput[] } | { scenarios: number; seed: string; scenario?: string })
export interface BacktestPlaybookBreakdown {
  trades: number; open_trades: number; expectancy: string | null; win_rate: number | null; adherence: number | null
  average_return_on_buying_power?: number | null
  realised_pnl: string; marked_pnl: string | null
  entry_reasons: Record<string, string>; rule_trips: BacktestResult["rule_trips"]
}
export interface BacktestIdentity {
  mode?: "joint"; playbook?: { id: string; version?: number; name?: string }
  playbooks?: { id: string; version?: number; name?: string }[]
}
export interface BacktestResult {
  started: string; ended: string
  pnl: string | null; last_mark_pnl: string; valuation_complete: boolean; quality_flags: string[]
  max_drawdown: string; min_floor_distance: string | null; outcome: "passed" | "failed" | "open"; decision: string
  rule_trips: { time: string; type: string; detail: unknown; playbook?: string }[]; trades: PlaybookTrade[]
  fills: { id: string; order: string; symbol: string; side: string; quantity: number; price: string; fee: string; time: string }[]
  stock_fills: { id: string; symbol: string; shares: number; price: string; time: string; option: string }[]
  stock_trades: { symbol: string; opened: string; closed: string | null; net: string; shares: number }[]
  per_playbook?: Record<string, BacktestPlaybookBreakdown>
  adherence: number | null; entry_reasons: Record<string, string>; open_positions: number; journal: string; journal_head: string
}
export interface BacktestDistribution {
  values: string[]; min: string | null; p25: string | null; median: string | null; p75: string | null; max: string | null; mean: string | null
}
export interface BacktestEvaluationDay {
  date: string; day_index: number; ended: string
  start_balance: string; end_balance: string; start_equity: string; end_equity: string
  pnl: string | null; valuation_complete: boolean; floor: string | null; floor_distance: string | null
  target: string; target_progress: string; peak: string; day_lock: string
  outcome: "open" | "passed" | "failed"; decision: string; decision_code: string
  rule_trips: BacktestResult["rule_trips"]; trades_opened: number; trades_closed: number
}
export interface BacktestReport {
  schema: number; simulated: true; label: string; status: "completed" | "cancelled" | "failed"; playbook?: Playbook
  mode?: "joint"; playbooks?: Playbook[]
  input_set?: { date: string; input: Record<string, unknown> | null }[]
  days: ((BacktestResult & { date: string; input: { kind: "scenario" | "recording"; id?: string; name?: string; seed?: string | number } }) | null)[]
  attempts: (BacktestResult & { first_day: number; last_day: number; days: number; day_rows?: BacktestEvaluationDay[] })[]
  errors: { day: number | null; message: string }[]
  summary: { per_playbook?: Record<string, BacktestPlaybookBreakdown>; daily_pnl: BacktestDistribution; daily_drawdown: BacktestDistribution; worst_days: number[]
    completed_days: number; marked_days: number; trades: number; expectancy: string | null; win_rate: number | null; day_win_rate: number | null
    /** Mean return on buying power of the closed trades; absent from older servers. */
    average_return_on_buying_power?: number | null
    attempts: number; passed: number; failed: number; open: number; pass_rate: number | null }
}
export interface BacktestState extends BacktestIdentity {
  id: string; status: "running" | "cancelling" | "cancelled" | "completed" | "failed" | "interrupted"
  /** Compact saved report identity and stats on GET /api/backtests. */
  summary?: BacktestReport["summary"]
  keep?: boolean; bytes?: number
  phase: string; directory?: string; completed: number; total: number; label: string; error?: string; report: BacktestReport | null
}
export interface BacktestListing { active: string | null; runs: BacktestState[]; label: string }

export interface BacktestComparison {
  label: string; different_inputs: boolean; different_plans: boolean; incomplete_inputs: boolean; apples_to_oranges: boolean
  runs: (BacktestIdentity & { id: string; status: BacktestState["status"]
    /** Reproducible SessionConfig retains its journal encoding, as in saved reports. */
    plan: { rules?: { plan?: string }; [key: string]: unknown }
    input_set: { date: string; input: Record<string, unknown> | null }[]; summary: BacktestReport["summary"] })[]
  daily: { date: string; pnl: Record<string, string | null> }[]
  combined: { label: string; includes_joint?: boolean; daily_pnl: BacktestDistribution
    curve: { date: string; pnl: string | null; cumulative: string | null; contributors: number }[]
    max_drawdown: string | null; worst_days: { date: string; pnl: string | null; cumulative: string | null; contributors: number }[]
    day_win_rate: number | null }
}
