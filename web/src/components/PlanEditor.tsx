import type { ReactNode } from "react"
import type { AccountRules, DailyLossBasis, Money, Plan } from "../api/trading-types"
import { dailyLossBasisText, dayEnd } from "../lib/plan-rules"
import { validMoney } from "../lib/trading"

/** The custom plan form: text fields as typed, choices as their API words. */
export interface PlanForm {
  name: string
  phase: AccountRules["phase"]
  qualifying_profit: string
  qualifying_days: string
  withdrawal_percent: string
  split_percent: string
  payout_minimum: string
  payout_caps: string
  payout_consistency_percents: string
  payout_buffer: string
  buffer_payouts: string
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
  max_contracts_held: string
  require_stop_loss: "yes" | "no"
  max_trade_risk: string
  max_trade_risk_percent: string
  strategies: "buy_only" | "defined_risk" | "any"
}

/** A form that starts from a preset's own rules, under a name of its own. */
export function planForm(plan: Pick<Plan, "initial_cash" | "rules">): PlanForm {
  const r = plan.rules
  const p = r.payouts
  return {
    phase: r.phase,
    qualifying_profit: p?.qualifying_profit ?? "0", qualifying_days: String(p?.qualifying_days ?? 1),
    withdrawal_percent: String(p?.withdrawal_percent ?? 50), split_percent: String(p?.split_percent ?? 80),
    payout_minimum: p?.minimum ?? "0", payout_caps: p?.caps.join(", ") ?? "",
    payout_consistency_percents: p?.consistency_percents?.join(", ") ?? "",
    payout_buffer: p?.buffer ?? "0", buffer_payouts: String(p?.buffer_payouts ?? 0),
    name: "Custom plan", initial_cash: plan.initial_cash,
    profit_target: r.profit_target ?? "", profit_basis: r.profit_basis ?? "equity",
    max_drawdown: r.max_drawdown ?? "", drawdown_mode: r.drawdown_mode,
    lock: r.lock_at_start ? "start" : r.lock_balance ? "balance" : "none", lock_balance: r.lock_balance ?? "",
    daily_loss_limit: r.daily_loss_limit ?? "", daily_loss_basis: r.daily_loss_basis ?? "equity", daily_loss_action: r.daily_loss_action ?? "lock",
    consistency_percent: r.consistency_percent ? String(r.consistency_percent) : "", consistency_basis: r.consistency_basis ?? "total",
    min_trading_days: r.min_trading_days ? String(r.min_trading_days) : "",
    min_profitable_days: r.min_profitable_days ? String(r.min_profitable_days) : "",
    profitable_day_profit: r.profitable_day_profit ?? "", day_end: dayEnd(r),
    max_contracts_held: r.max_contracts_held ? String(r.max_contracts_held) : "",
    require_stop_loss: r.require_stop_loss ? "yes" : "no", max_trade_risk: r.max_trade_risk ?? "",
    max_trade_risk_percent: r.max_trade_risk_percent ? String(r.max_trade_risk_percent) : "",
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
  for (const [label, value, required] of [["Starting balance", form.initial_cash, true], ["Profit target", form.phase === "funded" ? "" : form.profit_target],
    ["Max drawdown", form.max_drawdown], ["Lock balance", form.lock === "balance" ? form.lock_balance : ""],
    ["Maximum trade risk", form.max_trade_risk], ["Daily loss limit", form.daily_loss_limit], ["Profitable-day profit", form.phase === "funded" ? "" : form.profitable_day_profit]] as const)
    if ((required || value.trim() !== "") && !validMoney(value.trim())) return { error: `${label} must be a dollar amount` }
  if (!(Number(form.initial_cash) > 0)) return { error: "The starting balance must be above zero" }
  if (form.phase === "evaluation") for (const [label, value, most] of [["Consistency", form.consistency_percent, 100], ["Minimum trading days", form.min_trading_days, 366],
    ["Minimum profitable days", form.min_profitable_days, 366]] as const) {
    const n = count(value)
    if (!Number.isInteger(n) || n < 0 || n > most) return { error: `${label} must be a whole number from 0 to ${most}` }
  }
  for (const [label, value, most] of [["Maximum contracts held", form.max_contracts_held, 100000],
    ["Maximum trade risk %", form.max_trade_risk_percent, 100]] as const) {
    const n = count(value)
    if (!Number.isInteger(n) || n < 0 || n > most) return { error: `${label} must be a whole number from 0 to ${most}` }
  }
  if (!/^([01]\d|2[0-4]):[0-5]\d$/.test(form.day_end) || form.day_end < "16:15" || form.day_end > "24:00")
    return { error: "The trading day ends between 16:15 and 24:00" }
  const drawdown = amount(form.max_drawdown)
  if (form.lock === "balance" && !amount(form.lock_balance)) return { error: "Enter the balance the floor locks at" }
  const trailing = drawdown != null && form.drawdown_mode !== "static"
  let payouts: AccountRules["payouts"] = null
  if (form.phase === "funded") {
    for (const [label, value] of [["Qualifying day profit", form.qualifying_profit], ["Minimum payout", form.payout_minimum],
      ["Payout buffer", form.payout_buffer]] as const)
      if (!validMoney(value.trim()) || Number(value) < 0) return { error: `${label} must be a nonnegative dollar amount` }
    for (const [label, value, min, max] of [["Qualifying days", form.qualifying_days, 1, 366],
      ["Withdrawal percent", form.withdrawal_percent, 0, 100], ["Trader split percent", form.split_percent, 0, 100],
      ["Buffer payouts", form.buffer_payouts, 0, 100]] as const)
      if (!Number.isInteger(count(value)) || count(value) < min || count(value) > max)
        return { error: `${label} must be a whole number from ${min} to ${max}` }
    const list = (value: string) => value.trim() ? value.split(",").map((v) => v.trim()) : []
    const percents = list(form.payout_consistency_percents)
    if (percents.length > 64 || percents.some((v) => !/^\d+$/.test(v) || Number(v) < 1 || Number(v) > 100))
      return { error: "Payout consistency must be up to 64 comma-separated whole percentages from 1 to 100" }
    const caps = list(form.payout_caps)
    if (caps.length > 64 || caps.some((v) => !validMoney(v) || Number(v) <= 0))
      return { error: "Payout caps must be up to 64 comma-separated positive dollar amounts" }
    payouts = { qualifying_profit: form.qualifying_profit.trim(), qualifying_days: count(form.qualifying_days),
      withdrawal_percent: count(form.withdrawal_percent), split_percent: count(form.split_percent), minimum: form.payout_minimum.trim(),
      caps, consistency_percents: percents.map(Number), buffer: form.payout_buffer.trim(), buffer_payouts: count(form.buffer_payouts) }
  }
  const rules: AccountRules = {
    ...base, plan: name, plan_id: null, phase: form.phase, payouts,
    profit_target: form.phase === "funded" ? null : amount(form.profit_target), profit_basis: form.profit_basis,
    max_drawdown: drawdown, drawdown_mode: form.drawdown_mode,
    lock_at_start: trailing && form.lock === "start", lock_balance: trailing && form.lock === "balance" ? amount(form.lock_balance) : null,
    daily_loss_limit: amount(form.daily_loss_limit), daily_loss_basis: form.daily_loss_basis, daily_loss_action: form.daily_loss_action,
    consistency_percent: form.phase === "funded" ? 0 : count(form.consistency_percent), consistency_basis: form.consistency_basis,
    min_trading_days: form.phase === "funded" ? 0 : count(form.min_trading_days),
    min_profitable_days: form.phase === "funded" ? 0 : count(form.min_profitable_days),
    profitable_day_profit: form.phase === "funded" ? null : amount(form.profitable_day_profit), day_end: form.day_end,
    max_contracts_held: count(form.max_contracts_held), require_stop_loss: form.require_stop_loss === "yes",
    max_trade_risk: amount(form.max_trade_risk), max_trade_risk_percent: count(form.max_trade_risk_percent),
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

/** Edits a custom plan's rules; the dialog submits it. */
export function PlanEditor({ form, onChange, disabled }: { form: PlanForm; onChange: (form: PlanForm) => void; disabled?: boolean }) {
  const set = <K extends keyof PlanForm>(key: K) => (value: PlanForm[K]) => onChange({ ...form, [key]: value })
  const text = (key: keyof PlanForm, placeholder = "") => <input className="trade-input w-full" value={form[key]} placeholder={placeholder}
    disabled={disabled} inputMode={key === "name" || key === "day_end" || key === "payout_caps" || key === "payout_consistency_percents" ? "text" : "decimal"}
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
      <Field label="Phase">{choice("phase", [["evaluation", "Evaluation"], ["funded", "Funded (simulated payouts)"]])}</Field>
      {form.phase === "evaluation" && <Field label="Profit target" hint="Blank for none">{text("profit_target", "none")}</Field>}
      {form.phase === "evaluation" && <Field label="Profit counts on">{choice("profit_basis", [["equity", "Marked equity"], ["balance", "Closed balance (positions closed)"]])}</Field>}
      <Field label="Max drawdown" hint="Blank for no floor">{text("max_drawdown", "none")}</Field>
      <Field label="Floor">{choice("drawdown_mode", [["intraday", "Trails every new high"], ["end_of_day", "Trails each close"], ["static", "Static: never moves"]])}</Field>
      {trailing && <Field label="Floor locks">{choice("lock", [["none", "Never"], ["start", "At the starting balance"], ["balance", "At a balance"]])}</Field>}
      {trailing && form.lock === "balance" && <Field label="Lock balance">{text("lock_balance")}</Field>}
      <Field label="Daily loss limit" hint="Blank for none">{text("daily_loss_limit", "none")}</Field>
      <Field label="Measured from">{choice("daily_loss_basis", (Object.keys(dailyLossBasisText) as DailyLossBasis[]).map((b) => [b, dailyLossBasisText[b]]))}</Field>
      <Field label="Reaching it">{choice("daily_loss_action", [["lock", "Closes positions and locks the day"], ["fail", "Fails the attempt"]])}</Field>
      <Field label="Trading day ends (ET)" hint="HH:MM, 16:15 to 24:00">{text("day_end")}</Field>
      {form.phase === "evaluation" && <>
        <Field label="Consistency: best day at most %" hint="Blank for none">{text("consistency_percent", "none")}</Field>
        <Field label="Of">{choice("consistency_basis", [["total", "The total profit"], ["positive_days", "The profitable days' total"]])}</Field>
        <Field label="Minimum trading days">{text("min_trading_days", "0")}</Field>
        <Field label="Minimum profitable days">{text("min_profitable_days", "0")}</Field>
        <Field label="A profitable day makes at least" hint="Blank counts any profit">{text("profitable_day_profit", "any")}</Field>
      </>}
      {form.phase === "funded" && <>
        <Field label="Qualifying day profit" hint="Net realised profit after fees">{text("qualifying_profit")}</Field>
        <Field label="Qualifying days per payout">{text("qualifying_days")}</Field>
        <Field label="Withdrawal percent">{text("withdrawal_percent")}</Field>
        <Field label="Trader split percent">{text("split_percent")}</Field>
        <Field label="Minimum payout">{text("payout_minimum")}</Field>
        <Field label="Payout caps" hint="Comma-separated dollars by payout number; last repeats; blank for none">{text("payout_caps")}</Field>
        <Field label="Payout consistency percentages" hint="Best day as a share of cycle net profit. Comma-separated 1–100; last repeats; blank for none">{text("payout_consistency_percents", "20, 25, 30")}</Field>
        <Field label="Payout buffer" hint="Equity to keep above the starting balance; 0 disables">{text("payout_buffer")}</Field>
        <Field label="Buffer payouts" hint="First N payouts; 0 applies to every payout">{text("buffer_payouts")}</Field>
      </>}
      <Field label="Maximum contracts held" hint="Held options plus working entries; shares excluded. Blank for none.">{text("max_contracts_held", "none")}</Field>
      <Field label="Stop-loss required">{choice("require_stop_loss", [["no", "Optional"], ["yes", "Required on every entry"]])}</Field>
      <Field label="Maximum trade risk" hint="Dollars before fees. Blank for none.">{text("max_trade_risk", "none")}</Field>
      <Field label="Maximum trade risk %" hint="Of room to the plan floor; ignored without a floor. The tighter cap applies.">{text("max_trade_risk_percent", "none")}</Field>
      <Field label="Strategies">{choice("strategies", [["buy_only", "Buy only, single leg"], ["defined_risk", "Defined risk"], ["any", "Any"]])}</Field>
    </fieldset>
  )
}
