import type { AccountRules, DailyLossBasis, Evaluation, Money, Objective } from "../api/trading-types"
import { formatMoney, subtractMoney } from "./trading"

/** "18:00" as "6:00 pm"; "24:00" as "midnight". */
export function clockText(clock: string): string {
  const [hours = 0, minutes = 0] = clock.split(":").map(Number)
  if (hours === 24 || hours === 0) return "midnight"
  return `${hours % 12 || 12}:${String(minutes).padStart(2, "0")} ${hours < 12 ? "am" : "pm"}`
}
/** When the plan's trading day ends, New York time; servers before plan days end it at 17:00. */
export function dayEnd(rules: Pick<AccountRules, "day_end">): string { return rules.day_end ?? "17:00" }

export const dailyLossBasisText: Record<DailyLossBasis, string> = {
  equity: "the day's opening equity",
  balance: "the day's opening balance",
  higher: "the higher of the day's opening balance and equity",
  peak: "the day's equity high",
}

/** The floor in a few words: static, or trailing and where it locks. */
export function drawdownFact(rules: AccountRules, initialCash: Money): string {
  if (!rules.max_drawdown) return "No drawdown floor"
  const amount = formatMoney(rules.max_drawdown, 0)
  if (rules.drawdown_mode === "static") return `${amount} static drawdown: the floor stays at ${formatMoney(subtractMoney(initialCash, rules.max_drawdown), 0)}`
  const lock = rules.lock_at_start ? ", locks at the starting balance" : rules.lock_balance ? `, locks at ${formatMoney(rules.lock_balance, 0)}` : ""
  return `${amount} trailing drawdown (${rules.drawdown_mode === "intraday" ? "intraday" : "end of day"})${lock}`
}
/** How the floor moves, for plan tables. */
export function floorMoves(rules: AccountRules): string {
  if (!rules.max_drawdown) return "—"
  if (rules.drawdown_mode === "static") return "Never (static)"
  const moves = rules.drawdown_mode === "intraday" ? "Every new high" : "At each close"
  return rules.lock_at_start ? `${moves}, to the start` : moves
}
export function dailyLossFact(rules: AccountRules): string | null {
  if (!rules.daily_loss_limit) return null
  return `${formatMoney(rules.daily_loss_limit, 0)} daily loss limit from ${dailyLossBasisText[rules.daily_loss_basis ?? "equity"]}: ${
    rules.daily_loss_action === "fail" ? "fails the attempt" : "closes every position and locks the day"}`
}
/** The pass conditions beyond the target, in a few words each. */
export function objectiveFacts(rules: AccountRules): string[] {
  const facts: string[] = []
  if (rules.min_trading_days) facts.push(`At least ${rules.min_trading_days} trading ${rules.min_trading_days === 1 ? "day" : "days"}`)
  if (rules.min_profitable_days) facts.push(`At least ${rules.min_profitable_days} profitable ${rules.min_profitable_days === 1 ? "day" : "days"}${
    rules.profitable_day_profit ? ` of ${formatMoney(rules.profitable_day_profit, 0)}+` : ""}`)
  if (rules.consistency_percent) facts.push(`Best day at most ${rules.consistency_percent}% of ${
    rules.consistency_basis === "positive_days" ? "the profitable days' total" : "the total profit"}`)
  return facts
}
export function targetFact(rules: AccountRules): string {
  if (!rules.profit_target) return rules.phase === "funded" ? "Funded: no profit target" : "No profit target"
  return `${formatMoney(rules.profit_target, 0)} profit target${rules.profit_basis === "balance" ? " on the closed balance" : ""}`
}
export function dayEndFact(rules: AccountRules): string | null {
  return dayEnd(rules) === "17:00" ? null : `Trading day ends at ${clockText(dayEnd(rules))} ET`
}

export const objectiveLabels: Record<string, string> = {
  PROFIT_TARGET: "Profit target",
  MIN_TRADING_DAYS: "Trading days",
  MIN_PROFITABLE_DAYS: "Profitable days",
  CONSISTENCY: "Consistency",
}
/** An objective's standing in a few characters: dollars, days or percent. */
export function objectiveValue(o: Objective): string {
  if (o.code === "PROFIT_TARGET") return `${formatMoney(o.actual?.toFixed(2))} of ${formatMoney(o.required.toFixed(2), 0)}`
  if (o.code === "CONSISTENCY") return `${o.actual == null ? "—" : `${Math.round(o.actual)}%`} of ${o.required}% max`
  return `${o.actual ?? 0} of ${o.required}`
}
export const decisionLabels: Record<string, string> = {
  PROFIT_TARGET: "profit target",
  DRAWDOWN_FLOOR: "drawdown floor",
  DAILY_LOSS_LIMIT: "daily loss limit",
}
/** Why an attempt ended, as a short label: "daily loss limit". */
export function decisionLabel(code: string | null | undefined): string | null {
  return code ? decisionLabels[code] ?? code.toLowerCase().replaceAll("_", " ") : null
}
/** The plan's daily-loss room as a share of the limit, for meters; null without a limit. */
export function dailyLossShare(e: Pick<Evaluation, "daily_loss">): number | null {
  const d = e.daily_loss
  if (!d) return null
  const room = Number(d.room), limit = Number(d.limit)
  return Number.isFinite(room) && limit > 0 ? Math.max(0, Math.min(1, room / limit)) : null
}
/** The order tickets' notice while a plan limit locks the day. */
export const dayLockNotice = "The plan's daily loss limit locked trading until the next trading day · reduce-only: closing orders still work."
