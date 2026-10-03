import type { Account, AccountRules, Plan } from "../api/trading-types"
import { compareMoney, sumMoney } from "./trading"

const ignored = new Set(["plan_id", "slippage_ticks", "fill_latency_ms", "impact_ticks", "inside_fill_percent", "fees",
  "margin", "account_type", "house_margin_percent", "pm_vol_shock", "evaluation_fee", "reset_fee", "activation_fee", "max_resets"])
const moneyFields = new Set(["profit_target", "max_drawdown", "lock_balance", "daily_loss_limit", "profitable_day_profit", "max_trade_risk"])
const canonicalMoney = (value: unknown) => sumMoney([typeof value === "string" ? value : "0"]).replace(/\.?0+$/, "")
function payoutIdentity(rules: AccountRules) {
  if (!rules.payouts) return null
  const p = { consistency_percents: [], buffer: "0", buffer_payouts: 0, ...rules.payouts }
  return Object.fromEntries(Object.entries(p).sort(([a], [b]) => a.localeCompare(b)).map(([key, value]) => [key,
    key === "caps" ? (value as string[]).map(canonicalMoney)
      : ["qualifying_profit", "minimum", "buffer"].includes(key) ? canonicalMoney(value) : value]))
}
function identity(rules: AccountRules) {
  const defaults = { defined_risk: false, lock_at_start: false, profit_basis: "equity", daily_loss_limit: null,
    daily_loss_basis: "equity", daily_loss_action: "lock", consistency_percent: 0, consistency_basis: "total",
    min_trading_days: 0, min_profitable_days: 0, profitable_day_profit: null, day_end: "17:00",
    max_contracts_held: 0, require_stop_loss: false, max_trade_risk: null, max_trade_risk_percent: 0 }
  return Object.fromEntries(Object.entries({ ...defaults, ...rules }).filter(([key]) => !ignored.has(key))
    .sort(([a], [b]) => a.localeCompare(b)).map(([key, value]) => [key,
      key === "payouts" ? payoutIdentity(rules) : moneyFields.has(key) ? canonicalMoney(value) : value]))
}
/** Same starting balance and program rules; execution, margin and cost overrides do not buy a new plan. */
export function sameProgram(plan: Pick<Plan, "initial_cash" | "rules">, account: Account): boolean {
  return compareMoney(plan.initial_cash, account.evaluation.starting_balance) === 0 &&
    JSON.stringify(identity(plan.rules)) === JSON.stringify(identity(account.rules))
}
export function resetQuote(plan: Pick<Plan, "initial_cash" | "rules">, account: Account, id?: string) {
  const same = sameProgram(plan, account)
  const activation = !same && !!id && account.evaluation.status === "passed" && (account.next_plans?.includes(id) ?? false)
  const kind = same ? "reset" : activation ? "activation" : "evaluation"
  const used = same ? (account.costs?.resets_used ?? 0) : 0
  const terms = same && id ? account.rules : plan.rules
  const maximum = terms.max_resets ?? 0
  return { kind, fee: terms[`${kind}_fee`] ?? "0", blocked: same && maximum > 0 && used >= maximum,
    left: maximum === 0 ? null : Math.max(0, maximum - used - (same ? 1 : 0)) }
}
