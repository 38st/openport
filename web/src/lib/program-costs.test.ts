import { expect, it } from "vitest"
import { account, plans } from "../test/trading-fixtures"
import { resetQuote, sameProgram } from "./program-costs"
import { lockReason } from "./payouts"
import { customPlan, planForm } from "../components/PlanEditor"

it("quotes exact purchase, reset and activation costs without treating overrides as a new plan", () => {
  const plan = { ...plans[1]!, rules: { ...account.rules, evaluation_fee: "100.000001", reset_fee: "30.000003", activation_fee: "50", max_resets: 2 } }
  const current = { ...account, costs: { evaluation: "100.000001", reset: "30.000003", activation: "0", total: "130.000004",
    resets_used: 1, resets_left: 1, payouts_received: "0", net: "-130.000004", fee_charged: "30.000003", fee_kind: "reset" } }
  expect(sameProgram(plan, current)).toBe(true)
  expect(sameProgram({ ...plan, rules: { ...plan.rules, slippage_ticks: 2, margin: "portfolio", max_resets: 3 } }, current)).toBe(true)
  expect(resetQuote(plan, current)).toEqual({ kind: "reset", fee: "30.000003", left: 0, blocked: false })
  expect(resetQuote({ ...plan, rules: account.rules }, { ...current, rules: plan.rules }, plan.id))
    .toEqual({ kind: "reset", fee: "30.000003", left: 0, blocked: false })
  expect(resetQuote(plan, { ...current, costs: { ...current.costs, resets_used: 2 } }).blocked).toBe(true)
  const verification = { ...plan, id: "verify", rules: { ...plan.rules, phase: "verification" as const } }
  expect(resetQuote(verification, current)).toEqual({ kind: "evaluation", fee: "100.000001", left: 2, blocked: false })
  expect(resetQuote(verification, { ...current, next_plans: ["verify"], evaluation: { ...current.evaluation, status: "passed" } }, "verify"))
    .toEqual({ kind: "activation", fee: "50", left: 2, blocked: false })
})

it("uses server unlocks instead of a matching display name and allows restarting the entered step", () => {
  const verification = { ...plans[1]!, id: "verify", unlocked_by: plans[1]!.id, rules: { ...account.rules, phase: "verification" as const } }
  const list = [...plans, verification]
  const passed = { ...account, next_plans: [], evaluation: { ...account.evaluation, status: "passed" as const } }
  expect(lockReason(verification, list, passed)).toContain("Pass")
  expect(lockReason(verification, list, { ...passed, next_plans: ["verify"] })).toBeNull()
  expect(lockReason(verification, list, { ...account, rules: verification.rules })).toBeNull()
})

it("edits verification targets and all cost settings with exact decimal strings", () => {
  const base = plans[1]!
  const form = { ...planForm(base), phase: "verification" as const, evaluation_fee: "12.000001", reset_fee: "2.000001", activation_fee: "3", max_resets: "2" }
  expect(customPlan(form, base.rules)).toMatchObject({ rules: { phase: "verification", profit_target: base.rules.profit_target,
    evaluation_fee: "12.000001", reset_fee: "2.000001", activation_fee: "3", max_resets: 2 } })
  expect(customPlan({ ...form, max_resets: "1.5" }, base.rules)).toHaveProperty("error")
  expect(customPlan({ ...form, evaluation_fee: "-1" }, base.rules)).toHaveProperty("error")
  expect(customPlan({ ...form, min_trading_days: "367" }, base.rules)).toHaveProperty("error")
})

it("keeps grown funded accounts on their purchased plan and compares scaling money exactly", () => {
  const size_scaling = { profit_percent: 1, payouts: 0, days: 1, increase_percent: 25, max_balance: "200000.000001" }
  const rules = { ...account.rules, phase: "funded" as const, profit_target: null, max_drawdown: "1000.030001",
    daily_loss_limit: "500.030001", lock_balance: "100000", scaling: [{ profit: "0", contracts: 2 }, { profit: "100", contracts: 4 }],
    size_scaling, evaluation_fee: "100.000001", reset_fee: "25.000002", max_resets: 2 }
  const plan = { ...plans[1]!, initial_cash: "100000", rules, unlocked_by: "verification" }
  const current = { ...account, rules: { ...rules, max_drawdown: "1250.03", daily_loss_limit: "625.03", lock_balance: "125000",
    scaling: [{ contracts: 2, profit: "0.00" }, { contracts: 4, profit: "100.000000" }],
    size_scaling: { max_balance: "200000.000001", increase_percent: 25, days: 1, payouts: 0, profit_percent: 1 } },
    evaluation: { ...account.evaluation, starting_balance: "125000", size_scaling: {
      size: "125000", original: "100000", original_max_drawdown: "1000.030001", original_daily_loss_limit: "500.030001",
      max_balance: "200000.000001", period_started: "2026-09-24", period_days: 0, days_required: 1,
      period_profit: "0", profit_required: "1250", period_payouts: 0, payouts_required: 0,
      next_size: "150000", history: [{ day: "2026-09-24", old: "100000", size: "125000" }],
    } }, costs: { evaluation: "100.000001", reset: "50.000004", activation: "0", total: "150.000005",
      resets_used: 2, resets_left: 0, payouts_received: "0", net: "-150.000005", fee_charged: "25.000002", fee_kind: "reset" } }
  expect(sameProgram(plan, current)).toBe(true)
  expect(lockReason(plan, [plan], current)).toBeNull()
  expect(resetQuote(plan, current, plan.id)).toEqual({ kind: "reset", fee: "25.000002", blocked: true, left: 0 })
  for (const patch of [{ scaling: [{ profit: "0", contracts: 3 }] },
    { size_scaling: { ...size_scaling, max_balance: "200000.000002" } },
    { max_drawdown: "1000.03" }]) {
    expect(sameProgram({ ...plan, rules: { ...rules, ...patch } }, current)).toBe(false)
  }
  expect(sameProgram({ ...plan, initial_cash: "125000", rules: current.rules }, current)).toBe(false)
  expect(sameProgram({ ...plans[1]!, rules: { ...account.rules, scaling: [], size_scaling: null } }, account)).toBe(true)
})
