import type { Decision, Order, OrderChange, Position } from "../api/trading-types"
import { compareMoney, formatMoney } from "./trading"

export const isOpen = (order: Order) => order.status === "working" || order.status === "partially_filled" || order.status === "armed"

/** Resting orders the trader placed can change: DAY limit orders, armed orders and bracket exits. */
export function editable(order: Order): boolean {
  return isOpen(order) && order.origin !== "system" &&
    (order.status === "armed" || (order.type === "limit" && order.time_in_force !== "ioc"))
}

/** The terms an order can change: a bracket exit's size follows its position; only armed orders move their trigger. */
export function editableFields(order: Order) {
  return { quantity: !order.role, limit: order.type === "limit", trigger: order.status === "armed" && order.trigger != null }
}

export interface OrderDraft { quantity: string; limit_price: string; trigger_level: string }

export function orderDraft(order: Order): OrderDraft {
  return { quantity: String(order.quantity), limit_price: order.limit_price ?? "", trigger_level: order.trigger?.level ?? "" }
}

const decimalText = /^-?\d+(\.\d{1,6})?$/

/** The terms that changed, the first problem with the draft, or nothing to send. */
export function orderChange(order: Order, draft: OrderDraft): { change: OrderChange } | { error: string } | { unchanged: true } {
  const fields = editableFields(order)
  const change: OrderChange = {}
  if (fields.quantity) {
    const quantity = Number(draft.quantity)
    if (!/^\d+$/.test(draft.quantity.trim()) || !Number.isSafeInteger(quantity) || quantity <= 0)
      return { error: `Enter a whole number of ${order.legs ? "units" : "contracts"}` }
    if (quantity <= order.filled_quantity)
      return { error: `Keep more than the ${order.filled_quantity} already filled, or cancel the rest of the order` }
    if (quantity !== order.quantity) change.quantity = quantity
  }
  if (fields.limit) {
    const text = draft.limit_price.trim()
    if (!decimalText.test(text)) return { error: "Enter a limit price" }
    if (!order.legs && Number(text) <= 0) return { error: "The limit price must be positive" }
    if (compareMoney(text, order.limit_price) !== 0) change.limit_price = text
  }
  if (fields.trigger) {
    const text = draft.trigger_level.trim()
    if (!decimalText.test(text) || (order.trigger?.source !== "combo" && Number(text) <= 0)) return { error: order.trigger?.source === "combo" ? "Enter a signed combo net level" : "Enter a positive trigger level" }
    if (compareMoney(text, order.trigger?.level) !== 0) change.trigger_level = text
  }
  return Object.keys(change).length ? { change } : { unchanged: true }
}

/** The side and size of the order that closes a position: "Sell 2" for a long, "Buy 1" for a short. */
export const closingAction = (position: Position) => `${position.quantity > 0 ? "Sell" : "Buy"} ${Math.abs(position.quantity)}`

/**
 * What flattening does, as the server will: cancel the open orders in scope, then
 * close each position in scope that can still trade, short positions first.
 */
export function flattenPlan(positions: readonly Position[], orders: readonly Order[], underlying: string | null) {
  const inScope = (symbol: string) => underlying == null || symbol === underlying
  const closing = positions.filter((p) => p.quantity !== 0 && !p.awaiting_settlement && inScope(p.underlying))
    .sort((a, b) => Number(a.quantity > 0) - Number(b.quantity > 0))
  const cancelling = orders.filter((o) => isOpen(o) && inScope(o.underlying))
  return { closing, cancelling, exits: cancelling.filter((o) => o.role != null).length }
}

/** Underlyings with something to act on, in the order they first appear. */
export function underlyingsOf(items: readonly { underlying: string }[]): string[] {
  return [...new Set(items.map((item) => item.underlying))]
}

/** Checks whose evidence is in dollars; the others count contracts, dollar delta or vega. */
const dollarChecks = new Set(["DAILY_LOSS", "BUYING_POWER", "PRICE_BAND", "PROFIT_LOCK", "SOFT_FLOOR", "INVALID_PAYOUT"])
const units: Record<string, string> = { MAX_ORDER_CONTRACTS: " contracts", DELTA_LIMIT: " dollar delta", VEGA_LIMIT: " vega", TRADE_LIMIT: " opening trades", COOLDOWN: " minutes" }
/** The check behind a reason: a RISK_CHANGED cancel names it at the start of its message. */
export function checkCode(reason: Decision): string {
  if (reason.code !== "RISK_CHANGED") return reason.code
  const named = /^([A-Z_]+):/.exec(reason.message)
  return named ? named[1]! : reason.code
}
/** A reason's numbers: "$15.65 against a limit of $15.64 · aggregate", or null without any. */
export function reasonEvidence(reason: Decision | null | undefined): string | null {
  if (!reason) return null
  const code = checkCode(reason)
  const value = (n: number) => dollarChecks.has(code) ? formatMoney(n.toFixed(2))
    : `${n.toLocaleString("en-US", { maximumFractionDigits: 2 })}${units[code] ?? ""}`
  const parts: string[] = []
  if (reason.actual != null && reason.limit != null) parts.push(`${value(reason.actual)} against a limit of ${value(reason.limit)}`)
  else if (reason.actual != null) parts.push(value(reason.actual))
  if (reason.scope) parts.push(reason.scope)
  return parts.length ? parts.join(" · ") : null
}

/** A closing order's outcome: "Filled 2 at $4.00", or how much filled and why the rest did not, with its numbers. */
export function outcome(order: Order): string {
  if (order.status === "filled") return `Filled ${order.filled_quantity} at ${formatMoney(order.average_fill_price)}`
  if (order.status === "rejected" || order.status === "cancelled") {
    const evidence = reasonEvidence(order.reason)
    return `${order.filled_quantity ? `Filled ${order.filled_quantity} of ${order.quantity}, then ` : ""}${order.status === "rejected" ? "rejected" : "cancelled"}${order.reason ? `: ${order.reason.message}` : ""}${evidence ? ` (${evidence})` : ""}`
  }
  return order.status
}
