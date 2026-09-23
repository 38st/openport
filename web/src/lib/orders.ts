import type { Order, OrderChange, Position } from "../api/trading-types"
import { compareMoney, formatMoney } from "./trading"

export const isOpen = (order: Order) => order.status === "working" || order.status === "partially_filled" || order.status === "armed"

/** Resting orders the trader placed can change: DAY limit orders, armed orders and bracket exits. */
export function editable(order: Order): boolean {
  return isOpen(order) && order.origin !== "system" &&
    (order.status === "armed" || (order.type === "limit" && order.time_in_force === "day"))
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
    if (!decimalText.test(text) || Number(text) <= 0) return { error: "Enter a positive trigger level" }
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

/** A closing order's outcome: "Filled 2 at $4.00", or how much filled and why the rest did not. */
export function outcome(order: Order): string {
  if (order.status === "filled") return `Filled ${order.filled_quantity} at ${formatMoney(order.average_fill_price)}`
  if (order.status === "rejected" || order.status === "cancelled")
    return `${order.filled_quantity ? `Filled ${order.filled_quantity} of ${order.quantity}, then ` : ""}${order.status === "rejected" ? "rejected" : "cancelled"}${order.reason ? `: ${order.reason.message}` : ""}`
  return order.status
}
