import type { Position, Side } from "../api/trading-types"
import { nakedRequirement } from "./ticket"

/** An option position for margin, one per contract; mirrors the server's MarginLeg. */
export interface MarginPosition {
  symbol: string
  underlying: string
  /** Expiry id such as "2026-10-16PM"; positions with the same id expire together. */
  expiry: string
  type: "call" | "put"
  strike: number
  /** Signed contracts. */
  quantity: number
  /** Shorts: buy-back value of all the contracts, in dollars. */
  value: number
}
export type MarginContract = Pick<MarginPosition, "symbol" | "underlying" | "expiry" | "type" | "strike">

/** Held positions as the server values them: a short at its mark, or its entry credit without one. */
export function heldPositions(positions: Position[]): MarginPosition[] {
  return positions.filter((p) => p.quantity !== 0).map((p) => ({
    symbol: p.symbol, underlying: p.underlying, expiry: `${p.expiry}${p.settlement}`, type: p.type, strike: p.strike, quantity: p.quantity,
    value: p.quantity < 0 ? (p.mark != null ? Number(p.mark) * 100 * -p.quantity : -Number(p.basis)) : 0,
  }))
}

/** Trade `change` contracts: shorts that remain keep their share of the buy-back value; new shorts are valued at `price`. */
export function trade(book: MarginPosition[], contract: MarginContract, change: number, price: number): MarginPosition[] {
  const current = book.find((p) => p.symbol === contract.symbol)
  const quantity = current?.quantity ?? 0, value = current?.value ?? 0
  const next = quantity + change
  const rest = book.filter((p) => p.symbol !== contract.symbol)
  if (next === 0) return rest
  let nextValue = 0
  if (next < 0) {
    const kept = quantity < 0 ? Math.min(-quantity, -next) : 0
    nextValue = (kept > 0 ? (value * kept) / -quantity : 0) + price * 100 * (-next - kept)
  }
  return [...rest, { ...contract, quantity: next, value: nextValue }]
}

function naked(position: MarginPosition, n: number, spot: number | null | undefined) {
  return (position.value * n) / -position.quantity + n * nakedRequirement(position.type, position.strike, spot)
}
/**
 * Shorts paired with same-type longs that expire with them or later: the most
 * exposed shorts take the most protective eligible long (the earliest expiring
 * on a tie). A pair costs its width, never more than naked; the rest are naked.
 */
function verticals(positions: MarginPosition[], type: "call" | "put", spot: number | null | undefined) {
  const protects = (a: number, b: number) => (type === "put" ? a > b : a < b)
  const shorts = positions.filter((p) => p.type === type && p.quantity < 0)
    .sort((a, b) => (protects(a.strike, b.strike) ? -1 : protects(b.strike, a.strike) ? 1 : 0)).map((p) => ({ p, left: -p.quantity }))
  const longs = positions.filter((p) => p.type === type && p.quantity > 0).map((p) => ({ p, left: p.quantity }))
  let cost = 0
  for (const short of shorts) {
    while (short.left > 0) {
      let best: (typeof longs)[number] | null = null
      for (const long of longs)
        if (long.left > 0 && long.p.expiry >= short.p.expiry && (!best || protects(long.p.strike, best.p.strike) ||
            (long.p.strike === best.p.strike && long.p.expiry < best.p.expiry))) best = long
      if (!best) break
      const n = Math.min(short.left, best.left)
      const width = Math.max(0, type === "put" ? short.p.strike - best.p.strike : best.p.strike - short.p.strike)
      cost += Math.min(width * 100 * n, naked(short.p, n, spot))
      short.left -= n
      best.left -= n
    }
    if (short.left > 0) cost += naked(short.p, short.left, spot)
  }
  return cost
}
/** The group's worst loss at expiry, or null when net short calls leave it unbounded. */
function worstLoss(group: MarginPosition[]) {
  if (group.reduce((total, p) => total + (p.type === "call" ? p.quantity : 0), 0) < 0) return null
  const payoff = (spot: number) => group.reduce((total, p) =>
    total + p.quantity * 100 * Math.max(0, p.type === "call" ? spot - p.strike : p.strike - spot), 0)
  return Math.max(0, -Math.min(0, ...[0, ...group.map((p) => p.strike)].map(payoff)))
}
/**
 * Mirrors the server's margin_requirement: per underlying, the least of pairing
 * across expiries (a later long covers an earlier short) and taking each expiry
 * on its own, the lesser of its verticals and its bounded worst loss.
 */
export function marginRequirement(positions: MarginPosition[], spot: number | null | undefined): number {
  const group = <K,>(items: MarginPosition[], key: (p: MarginPosition) => K) => {
    const groups = new Map<K, MarginPosition[]>()
    for (const p of items) groups.set(key(p), [...(groups.get(key(p)) ?? []), p])
    return [...groups.values()]
  }
  let total = 0
  for (const all of group(positions.filter((p) => p.quantity !== 0), (p) => p.underlying)) {
    let separate = 0
    for (const expiry of group(all, (p) => p.expiry)) {
      const pairs = verticals(expiry, "put", spot) + verticals(expiry, "call", spot)
      const loss = worstLoss(expiry)
      separate += loss == null ? pairs : Math.min(pairs, loss)
    }
    total += Math.min(separate, verticals(all, "put", spot) + verticals(all, "call", spot))
  }
  return total
}

export interface PowerUse {
  /** Change in buying power, negative when the order uses it: fees plus any net cost. */
  effect: number
  /** Filling would reduce free buying power, so it needs buying power available. */
  uses: boolean
}
/**
 * Mirrors the server: fees, plus the change in the margin requirement from the
 * held positions to after the fill, plus the premium paid (less the premium
 * received). An order that frees more than it costs uses only its fees.
 */
export function powerUse(held: MarginPosition[], after: MarginPosition[], cash: number, fees: number, spot: number | null | undefined): PowerUse {
  const underlyings = new Set([...held, ...after].map((p) => p.underlying))
  const scope = (book: MarginPosition[]) => book.filter((p) => underlyings.has(p.underlying))
  const change = marginRequirement(scope(after), spot) - marginRequirement(scope(held), spot) + cash
  return { effect: -(fees + Math.max(0, change)), uses: fees + change > 0 }
}
/** A single-leg order's use of buying power against the held positions. */
export function orderPowerUse(order: { side: Side; quantity: number; price: number | null; fee: number }, contract: MarginContract,
  held: MarginPosition[], spot: number | null | undefined): PowerUse | null {
  const { side, quantity, price, fee } = order
  if (!Number.isSafeInteger(quantity) || quantity <= 0 || price == null || !Number.isFinite(price) || !Number.isFinite(fee)) return null
  const after = trade(held, contract, side === "buy" ? quantity : -quantity, price)
  return powerUse(held, after, (side === "buy" ? 1 : -1) * price * 100 * quantity, fee * quantity, spot)
}
