import { describe, expect, it } from "vitest"
import type { AccountRules } from "../api/trading-types"
import { customPlan, planForm } from "../components/PlanEditor"
import { account, fundedAccount } from "../test/trading-fixtures"
import { clockText, dailyLossFact, dayEndFact, decisionLabel, drawdownFact, floorMoves, objectiveFacts, objectiveValue, targetFact } from "./plan-rules"

const rules: AccountRules = { ...account.rules, buy_only: false }

describe("plan rule texts", () => {
  it("names static and locking floors, and older servers' floors as before", () => {
    expect(drawdownFact(rules, "100000.00")).toBe("$5,000 trailing drawdown (intraday)")
    expect(drawdownFact({ ...rules, drawdown_mode: "static" }, "100000.00")).toBe("$5,000 static drawdown: the floor stays at $95,000")
    expect(drawdownFact({ ...rules, drawdown_mode: "end_of_day", lock_at_start: true }, "100000.00"))
      .toBe("$5,000 trailing drawdown (end of day), locks at the starting balance")
    expect(drawdownFact({ ...rules, lock_balance: "100000.00" }, "100000.00")).toBe("$5,000 trailing drawdown (intraday), locks at $100,000")
    expect(floorMoves({ ...rules, drawdown_mode: "static" })).toBe("Never (static)")
    expect(floorMoves({ ...rules, drawdown_mode: "end_of_day", lock_at_start: true })).toBe("At each close, to the start")
    expect(floorMoves({ ...rules, max_drawdown: null })).toBe("—")
  })
  it("states the daily loss limit, objectives, target basis and day end only when set", () => {
    expect(dailyLossFact(rules)).toBeNull()
    expect(objectiveFacts(rules)).toEqual([])
    expect(dayEndFact(rules)).toBeNull()
    expect(targetFact(rules)).toBe("$10,000 profit target")
    const plan: AccountRules = { ...rules, profit_basis: "balance", daily_loss_limit: "2000.00", daily_loss_basis: "higher",
      daily_loss_action: "fail", min_trading_days: 4, min_profitable_days: 1, profitable_day_profit: "500.00",
      consistency_percent: 40, consistency_basis: "positive_days", day_end: "18:00" }
    expect(targetFact(plan)).toBe("$10,000 profit target on the closed balance")
    expect(dailyLossFact(plan)).toBe("$2,000 daily loss limit from the higher of the day's opening balance and equity: fails the attempt")
    expect(dailyLossFact({ ...plan, daily_loss_action: "lock", daily_loss_basis: "peak" }))
      .toBe("$2,000 daily loss limit from the day's equity high: closes every position and locks the day")
    expect(objectiveFacts(plan)).toEqual(["At least 4 trading days", "At least 1 profitable day of $500+", "Best day at most 40% of the profitable days' total"])
    expect(dayEndFact(plan)).toBe("Trading day ends at 6:00 pm ET")
    expect(clockText("24:00")).toBe("midnight")
    expect(clockText("16:15")).toBe("4:15 pm")
  })
  it("shows objectives' standing and decision codes in a trader's words", () => {
    expect(objectiveValue({ code: "PROFIT_TARGET", met: false, actual: 267.5, required: 10000, message: "" })).toBe("$267.50 of $10,000")
    expect(objectiveValue({ code: "MIN_TRADING_DAYS", met: false, actual: 2, required: 4, message: "" })).toBe("2 of 4")
    expect(objectiveValue({ code: "CONSISTENCY", met: false, actual: 66.66, required: 50, message: "" })).toBe("67% of 50% max")
    expect(objectiveValue({ code: "CONSISTENCY", met: true, actual: null, required: 50, message: "" })).toBe("— of 50% max")
    expect(decisionLabel("DAILY_LOSS_LIMIT")).toBe("daily loss limit")
    expect(decisionLabel("SOME_NEW_CODE")).toBe("some new code")
    expect(decisionLabel(null)).toBeNull()
  })
})

describe("custom plans", () => {
  it("start from a preset's rules and send every edited rule", () => {
    const form = { ...planForm({ initial_cash: "100000.00", rules }), name: "My plan", drawdown_mode: "static" as const, lock: "start" as const,
      daily_loss_limit: "1500", daily_loss_basis: "peak" as const, daily_loss_action: "fail" as const, consistency_percent: "40",
      min_trading_days: "3", min_profitable_days: "2", profitable_day_profit: "250", profit_basis: "balance" as const, day_end: "24:00" }
    const result = customPlan(form, rules)
    expect("error" in result).toBe(false)
    if ("error" in result) return
    expect(result.initial_cash).toBe("100000.00")
    expect(result.rules).toMatchObject({ plan: "My plan", phase: "evaluation", payouts: null, profit_target: "10000.00", max_drawdown: "5000.00",
      drawdown_mode: "static", lock_at_start: false, lock_balance: null, profit_basis: "balance", daily_loss_limit: "1500",
      daily_loss_basis: "peak", daily_loss_action: "fail", consistency_percent: 40, consistency_basis: "total", min_trading_days: 3,
      min_profitable_days: 2, profitable_day_profit: "250", day_end: "24:00", buy_only: false, defined_risk: false,
      buying_power: true, expiry_cutoff_seconds: 300 })
    // A trailing floor keeps its lock; blank amounts turn rules off.
    const trailing = customPlan({ ...form, drawdown_mode: "end_of_day", daily_loss_limit: "", consistency_percent: "" }, rules)
    expect("error" in trailing ? null : trailing.rules).toMatchObject({ lock_at_start: true, daily_loss_limit: null, consistency_percent: 0 })
  })
  it("edits funded payout rules and validates consistency and buffer settings", () => {
    const base = fundedAccount.rules
    const form = { ...planForm({ initial_cash: "100000.00", rules: base }),
      payout_consistency_percents: "20, 25, 30", payout_buffer: "2100.000001", buffer_payouts: "3" }
    const result = customPlan(form, base)
    expect("error" in result ? result : result.rules).toMatchObject({ phase: "funded", profit_target: null,
      payouts: { ...base.payouts, consistency_percents: [20, 25, 30], buffer: "2100.000001", buffer_payouts: 3 } })
    for (const value of ["0", "101", "40.5", "20,", ",20", "abc", Array(65).fill("40").join(",")])
      expect(customPlan({ ...form, payout_consistency_percents: value }, base)).toHaveProperty("error")
    for (const value of ["-1", "101", "1.5", "abc"])
      expect(customPlan({ ...form, buffer_payouts: value }, base)).toHaveProperty("error")
    expect(customPlan({ ...form, payout_buffer: "-0.01" }, base)).toHaveProperty("error")
    const off = customPlan({ ...form, payout_consistency_percents: "", payout_buffer: "0", buffer_payouts: "0" }, base)
    expect("error" in off ? off : off.rules.payouts).toMatchObject({ consistency_percents: [], buffer: "0", buffer_payouts: 0 })
    const restored = planForm({ initial_cash: "100000.00", rules: { ...base,
      payouts: { ...base.payouts!, consistency_percents: [40], buffer: "2100.00", buffer_payouts: 2 } } })
    expect(restored).toMatchObject({ phase: "funded", payout_consistency_percents: "40", payout_buffer: "2100.00", buffer_payouts: "2" })
  })
  it("clears hidden pass objectives when converting an evaluation template to funded", () => {
    const form = { ...planForm({ initial_cash: "100000.00", rules }), phase: "funded" as const,
      consistency_percent: "40", min_trading_days: "4", min_profitable_days: "2", profitable_day_profit: "250" }
    const result = customPlan(form, rules)
    expect("error" in result ? result : result.rules).toMatchObject({ phase: "funded", profit_target: null,
      consistency_percent: 0, min_trading_days: 0, min_profitable_days: 0, profitable_day_profit: null })
    expect(customPlan({ ...form, consistency_percent: "x", profitable_day_profit: "x" }, rules)).not.toHaveProperty("error")
  })
  it("names the first problem before the server sees it", () => {
    const form = planForm({ initial_cash: "100000.00", rules })
    expect(customPlan({ ...form, name: " " }, rules)).toEqual({ error: "Name the plan" })
    expect(customPlan({ ...form, daily_loss_limit: "1,000" }, rules)).toEqual({ error: "Daily loss limit must be a dollar amount" })
    expect(customPlan({ ...form, consistency_percent: "120" }, rules)).toEqual({ error: "Consistency must be a whole number from 0 to 100" })
    expect(customPlan({ ...form, min_trading_days: "2.5" }, rules)).toEqual({ error: "Minimum trading days must be a whole number from 0 to 366" })
    expect(customPlan({ ...form, day_end: "16:00" }, rules)).toEqual({ error: "The trading day ends between 16:15 and 24:00" })
    expect(customPlan({ ...form, day_end: "24:30" }, rules)).toEqual({ error: "The trading day ends between 16:15 and 24:00" })
    expect(customPlan({ ...form, lock: "balance", lock_balance: "" }, rules)).toEqual({ error: "Enter the balance the floor locks at" })
    expect(customPlan({ ...form, initial_cash: "0" }, rules)).toEqual({ error: "The starting balance must be above zero" })
  })
})

it("edits trade-entry rules and validates their ranges", () => {
  const base = { ...rules, max_contracts_held: 5, require_stop_loss: true, max_trade_risk: "200.50", max_trade_risk_percent: 10 }
  const form = planForm({ initial_cash: "10000", rules: base })
  expect(form).toMatchObject({ max_contracts_held: "5", require_stop_loss: "yes", max_trade_risk: "200.50", max_trade_risk_percent: "10" })
  const result = customPlan(form, base)
  expect("error" in result ? result : result.rules).toMatchObject({ max_contracts_held: 5, require_stop_loss: true, max_trade_risk: "200.50", max_trade_risk_percent: 10 })
  for (const max_contracts_held of ["-1", "100001", "1.5"])
    expect(customPlan({ ...form, max_contracts_held }, base)).toHaveProperty("error")
  for (const max_trade_risk_percent of ["-1", "101", "0.5"])
    expect(customPlan({ ...form, max_trade_risk_percent }, base)).toHaveProperty("error")
  expect(customPlan({ ...form, max_trade_risk: "-1" }, base)).toHaveProperty("error")
  const off = customPlan({ ...form, max_contracts_held: "", require_stop_loss: "no", max_trade_risk: "", max_trade_risk_percent: "" }, base)
  expect("error" in off ? off : off.rules).toMatchObject({ max_contracts_held: 0, require_stop_loss: false, max_trade_risk: null, max_trade_risk_percent: 0 })
})
