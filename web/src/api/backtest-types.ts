import type { Playbook, PlaybookTrade } from "./playbook-types"
export type BacktestDayInput = { file: string } | { scenario: string; date?: string; seed: string }
export type BacktestStart = { playbook: string; plan: string; workers?: number } & (
  { days: BacktestDayInput[] } | { scenarios: number; seed: string; scenario?: string })
export interface BacktestResult {
  started: string; ended: string
  pnl: string | null; last_mark_pnl: string; valuation_complete: boolean; quality_flags: string[]
  max_drawdown: string; min_floor_distance: string | null; outcome: "passed" | "failed" | "open"; decision: string
  rule_trips: { time: string; type: string; detail: unknown }[]; trades: PlaybookTrade[]
  fills: { id: string; order: string; symbol: string; side: string; quantity: number; price: string; fee: string; time: string }[]
  stock_fills: { id: string; symbol: string; shares: number; price: string; time: string; option: string }[]
  stock_trades: { symbol: string; opened: string; closed: string | null; net: string; shares: number }[]
  adherence: number | null; entry_reasons: Record<string, string>; open_positions: number; journal: string; journal_head: string
}
export interface BacktestDistribution {
  values: string[]; min: string | null; p25: string | null; median: string | null; p75: string | null; max: string | null; mean: string | null
}
export interface BacktestReport {
  schema: number; simulated: true; label: string; status: "completed" | "cancelled" | "failed"; playbook: Playbook
  days: ((BacktestResult & { date: string; input: { kind: "scenario" | "recording"; id?: string; name?: string; seed?: string | number } }) | null)[]
  attempts: (BacktestResult & { first_day: number; last_day: number; days: number })[]
  errors: { day: number | null; message: string }[]
  summary: { daily_pnl: BacktestDistribution; daily_drawdown: BacktestDistribution; worst_days: number[]
    completed_days: number; marked_days: number; trades: number; expectancy: string | null; win_rate: number | null; day_win_rate: number | null
    /** Mean return on buying power of the closed trades; absent from older servers. */
    average_return_on_buying_power?: number | null
    attempts: number; passed: number; failed: number; open: number; pass_rate: number | null }
}
export interface BacktestState {
  id: string; status: "running" | "cancelling" | "cancelled" | "completed" | "failed" | "interrupted"
  phase: string; directory?: string; completed: number; total: number; label: string; error?: string; report: BacktestReport | null
}
export interface BacktestListing { active: string | null; runs: BacktestState[]; label: string }
