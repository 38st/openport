import type { Account, EvaluationDay, Money, PayoutStatus, Plan } from "../api/trading-types"
import { showFundedAccounts } from "./features"
import { compareMoney, formatMoney } from "./trading"

/** Plans the terminal offers: funded plans only when they are shown. */
export function offeredPlans(plans: Plan[]): Plan[] {
  return showFundedAccounts ? plans : plans.filter((p) => p.rules.phase !== "funded")
}
/** The Payouts page is shown with funded plans, or for an account that is already funded. */
export function payoutsVisible(account: Pick<Account, "rules"> | undefined): boolean {
  return showFundedAccounts || account?.rules.phase === "funded"
}

/** A payout number's cap: the last cap repeats; null when uncapped. */
export function payoutCap(caps: Money[], number: number): Money | null {
  return caps.length ? caps[Math.min(number, caps.length) - 1] ?? null : null
}

/** Funded plans unlock once the current attempt has passed the evaluation they name. */
export function lockReason(plan: Plan, plans: Plan[], account: Account | undefined): string | null {
  if (!plan.unlocked_by) return null
  const name = plans.find((p) => p.id === plan.unlocked_by)?.name ?? plan.unlocked_by
  const passed = account?.evaluation.status === "passed" && account.rules.plan === name
  return passed ? null : `Pass ${name} to unlock`
}

/** The funded plan that the current, passed evaluation unlocks. */
export function unlockedFundedPlan(plans: Plan[], account: Account | undefined): Plan | null {
  return plans.find((p) => p.unlocked_by != null && lockReason(p, plans, account) == null) ?? null
}

/** Finished days of the current payout cycle: each day counts toward the cycle in progress when it
 * closes, so the trading day of the last payout request and later days. */
export function cycleDays(account: Account): EvaluationDay[] {
  const last = account.evaluation.payouts.at(-1)
  return last ? account.evaluation.days.filter((d) => d.day >= last.day) : account.evaluation.days
}

export interface PayoutCheck { label: string; ok: boolean; value?: string }
/** Every payout requirement with its current standing, in the server's order. */
export function payoutChecks(status: PayoutStatus): PayoutCheck[] {
  const checks: PayoutCheck[] = [
    { label: "Funded account is active", ok: status.active },
    { label: "No open positions or working orders", ok: status.flat },
    { label: `Qualifying days of ${formatMoney(status.qualifying_profit, 0)}+ net realised profit`,
      ok: status.qualifying_days >= status.required_days, value: `${status.qualifying_days} of ${status.required_days}` },
  ]
  checks.push({ label: "Available payout meets the minimum",
    ok: (compareMoney(status.maximum, "0") ?? 0) > 0 && (compareMoney(status.maximum, status.minimum) ?? -1) >= 0,
    value: `${formatMoney(status.maximum)} of ${formatMoney(status.minimum)}` })
  return checks
}

/** A whole-cent amount within the accepted range, or why not. */
export function payoutAmountError(amount: string, status: PayoutStatus): string | null {
  if (!/^\d+(\.\d{1,2})?$/.test(amount.trim())) return "Enter an amount in dollars and cents"
  if ((compareMoney(amount.trim(), "0") ?? 0) <= 0) return "Enter an amount above zero"
  if ((compareMoney(amount.trim(), status.minimum) ?? -1) < 0) return `The minimum payout is ${formatMoney(status.minimum)}`
  if ((compareMoney(amount.trim(), status.maximum) ?? 1) > 0) return `The most you can request now is ${formatMoney(status.maximum)}`
  return null
}
