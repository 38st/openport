import type { ReactNode } from "react"
import type { AccountRules, DailyLossBasis, Money, Plan } from "../api/trading-types"
import { dailyLossBasisText, dayEnd } from "../lib/plan-rules"
import { validMoney } from "../lib/trading"

/** The custom plan form: text fields as typed, choices as their API words. */
export interface PlanForm {
  name: string
  initial_cash: string
  profit_target: string
  profit_basis: "equity" | "balance"
  max_drawdown: string
  drawdown_mode: AccountRules["drawdown_mode"]
  lock: "none" | "start" | "balance"
  lock_balance: string
  daily_loss_limit: string
  daily_loss_basis: DailyLossBasis
  daily_loss_action: "lock" | "fail"
  consistency_percent: string
  consistency_basis: "total" | "positive_days"
  min_trading_days: string
  min_profitable_days: string
  profitable_day_profit: string
  day_end: string
  strategies: "buy_only" | "defined_risk" | "any"
}

/** A form that starts from a preset's own rules, under a name of its own. */
export function planForm(plan: Pick<Plan, "initial_cash" | "rules">): PlanForm {
  const r = plan.rules
  return {
    name: "Custom plan", initial_cash: plan.initial_cash,
    profit_target: r.profit_target ?? "", profit_basis: r.profit_basis ?? "equity",
    max_drawdown: r.max_drawdown ?? "", drawdown_mode: r.drawdown_mode,
    lock: r.lock_at_start ? "start" : r.lock_balance ? "balance" : "none", lock_balance: r.lock_balance ?? "",
    daily_loss_limit: r.daily_loss_limit ?? "", daily_loss_basis: r.daily_loss_basis ?? "equity", daily_loss_action: r.daily_loss_action ?? "lock",
    consistency_percent: r.consistency_percent ? String(r.consistency_percent) : "", consistency_basis: r.consistency_basis ?? "total",
    min_trading_days: r.min_trading_days ? String(r.min_trading_days) : "",
    min_profitable_days: r.min_profitable_days ? String(r.min_profitable_days) : "",
    profitable_day_profit: r.profitable_day_profit ?? "", day_end: dayEnd(r),
    strategies: r.buy_only ? "buy_only" : r.defined_risk ? "defined_risk" : "any",
  }
}

const count = (text: string) => text.trim() === "" ? 0 : Number(text.trim())
const amount = (text: string): Money | null => text.trim() === "" || Number(text.trim()) === 0 ? null : text.trim()

/**
 * The reset request's balance and rules from the form, keeping the base preset's other
 * rules (buying power, margin, fills, expiry auto-close); or the first problem found.
 * The server checks every rule again.
 */
export function customPlan(form: PlanForm, base: AccountRules): { initial_cash: Money; rules: AccountRules } | { error: string } {
  const name = form.name.trim()
  if (!name) return { error: "Name the plan" }
  for (const [label, value, required] of [["Starting balance", form.initial_cash, true], ["Profit target", form.profit_target],
    ["Max drawdown", form.max_drawdown], ["Lock balance", form.lock === "balance" ? form.lock_balance : ""],
    ["Daily loss limit", form.daily_loss_limit], ["Profitable-day profit", form.profitable_day_profit]] as const)
    if ((required || value.trim() !== "") && !validMoney(value.trim())) return { error: `${label} must be a dollar amount` }
  if (!(Number(form.initial_cash) > 0)) return { error: "The starting balance must be above zero" }
  for (const [label, value, most] of [["Consistency", form.consistency_percent, 100], ["Minimum trading days", form.min_trading_days, 366],
    ["Minimum profitable days", form.min_profitable_days, 366]] as const) {
    const n = count(value)
    if (!Number.isInteger(n) || n < 0 || n > most) return { error: `${label} must be a whole number from 0 to ${most}` }
  }
  if (!/^([01]\d|2[0-4]):[0-5]\d$/.test(form.day_end) || form.day_end < "16:15" || form.day_end > "24:00")
    return { error: "The trading day ends between 16:15 and 24:00" }
  const drawdown = amount(form.max_drawdown)
  if (form.lock === "balance" && !amount(form.lock_balance)) return { error: "Enter the balance the floor locks at" }
  const trailing = drawdown != null && form.drawdown_mode !== "static"
  const rules: AccountRules = {
    ...base, plan: name, phase: "evaluation", payouts: null,
    profit_target: amount(form.profit_target), profit_basis: form.profit_basis,
    max_drawdown: drawdown, drawdown_mode: form.drawdown_mode,
    lock_at_start: trailing && form.lock === "start", lock_balance: trailing && form.lock === "balance" ? amount(form.lock_balance) : null,
    daily_loss_limit: amount(form.daily_loss_limit), daily_loss_basis: form.daily_loss_basis, daily_loss_action: form.daily_loss_action,
    consistency_percent: count(form.consistency_percent), consistency_basis: form.consistency_basis,
    min_trading_days: count(form.min_trading_days), min_profitable_days: count(form.min_profitable_days),
    profitable_day_profit: amount(form.profitable_day_profit), day_end: form.day_end,
    buy_only: form.strategies === "buy_only", defined_risk: form.strategies === "defined_risk",
  }
  return { initial_cash: form.initial_cash.trim(), rules }
}

function Field({ label, hint, children }: { label: string; hint?: string; children: ReactNode }) {
  return <label className="block min-w-0 space-y-1 text-xs">
    <span className="text-muted">{label}</span>
    {children}
    {hint && <span className="block text-[11px] text-faint">{hint}</span>}
  </label>
}

/** Edits a custom plan's evaluation rules; the dialog submits it. */
export function PlanEditor({ form, onChange, disabled }: { form: PlanForm; onChange: (form: PlanForm) => void; disabled?: boolean }) {
  const set = <K extends keyof PlanForm>(key: K) => (value: PlanForm[K]) => onChange({ ...form, [key]: value })
  const text = (key: keyof PlanForm, placeholder = "") => <input className="trade-input w-full" value={form[key]} placeholder={placeholder}
    disabled={disabled} inputMode={key === "name" || key === "day_end" ? "text" : "decimal"}
    onChange={(event) => set(key)(event.target.value as never)} />
  const choice = <K extends keyof PlanForm>(key: K, options: [PlanForm[K], string][]) => <select className="trade-input w-full"
    value={form[key]} disabled={disabled} onChange={(event) => set(key)(event.target.value as PlanForm[K])}>
    {options.map(([value, label]) => <option key={value} value={value}>{label}</option>)}
  </select>
  const trailing = form.drawdown_mode !== "static"
  return (
    <fieldset className="grid gap-3 rounded-md border border-border p-3 sm:grid-cols-2" disabled={disabled} aria-label="Custom plan rules">
      <Field label="Plan name" hint="Not a preset's name">{text("name")}</Field>
      <Field label="Starting balance">{text("initial_cash")}</Field>
      <Field label="Profit target" hint="Blank for none">{text("profit_target", "none")}</Field>
      <Field label="Profit counts on">{choice("profit_basis", [["equity", "Marked equity"], ["balance", "Closed balance (positions closed)"]])}</Field>
      <Field label="Max drawdown" hint="Blank for no floor">{text("max_drawdown", "none")}</Field>
      <Field label="Floor">{choice("drawdown_mode", [["intraday", "Trails every new high"], ["end_of_day", "Trails each close"], ["static", "Static: never moves"]])}</Field>
      {trailing && <Field label="Floor locks">{choice("lock", [["none", "Never"], ["start", "At the starting balance"], ["balance", "At a balance"]])}</Field>}
      {trailing && form.lock === "balance" && <Field label="Lock balance">{text("lock_balance")}</Field>}
      <Field label="Daily loss limit" hint="Blank for none">{text("daily_loss_limit", "none")}</Field>
      <Field label="Measured from">{choice("daily_loss_basis", (Object.keys(dailyLossBasisText) as DailyLossBasis[]).map((b) => [b, dailyLossBasisText[b]]))}</Field>
      <Field label="Reaching it">{choice("daily_loss_action", [["lock", "Closes positions and locks the day"], ["fail", "Fails the attempt"]])}</Field>
      <Field label="Trading day ends (ET)" hint="HH:MM, 16:15 to 24:00">{text("day_end")}</Field>
      <Field label="Consistency: best day at most %" hint="Blank for none">{text("consistency_percent", "none")}</Field>
      <Field label="Of">{choice("consistency_basis", [["total", "The total profit"], ["positive_days", "The profitable days' total"]])}</Field>
      <Field label="Minimum trading days">{text("min_trading_days", "0")}</Field>
      <Field label="Minimum profitable days">{text("min_profitable_days", "0")}</Field>
      <Field label="A profitable day makes at least" hint="Blank counts any profit">{text("profitable_day_profit", "any")}</Field>
      <Field label="Strategies">{choice("strategies", [["buy_only", "Buy only, single leg"], ["defined_risk", "Defined risk"], ["any", "Any"]])}</Field>
    </fieldset>
  )
}
