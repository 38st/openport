import type { OptionQuote } from "../api/types"
import type { Side } from "../api/trading-types"
import { nakedRequirement } from "./ticket"

export type Kind = "call" | "put"
/** A leg as the ticket holds it, with its latest quote. */
export interface StrategyLeg {
  symbol: string
  side: Side
  ratio: number
  type: Kind
  strike: number
  /** Expiry id; a strategy's legs share one in the ticket. */
  expiry: string
  quote: OptionQuote | null
}
export const MAX_LEGS = 4
export const MAX_RATIO = 10
const sign = (side: Side) => (side === "buy" ? 1 : -1)
const finite = (value: number | null | undefined): value is number => value != null && Number.isFinite(value)

/** Add a contract as a leg, flip it to the other side, or remove it when clicked again on its side. */
export function toggleLeg(legs: StrategyLeg[], leg: StrategyLeg): StrategyLeg[] {
  const at = legs.findIndex((l) => l.symbol === leg.symbol)
  if (at < 0) return legs.length >= MAX_LEGS ? legs : [...legs, leg]
  if (legs[at]!.side === leg.side) return legs.filter((_, i) => i !== at)
  return legs.map((l, i) => (i === at ? { ...l, side: leg.side } : l))
}

/** Net per unit, debit positive: `ask` pays the far sides (asks bought, bids sold), `bid` is the reverse. */
export function netQuote(legs: StrategyLeg[]): { bid: number | null; mid: number | null; ask: number | null } {
  let bid = 0, mid = 0, ask = 0
  for (const leg of legs) {
    const q = leg.quote
    if (!q || !finite(q.bid) || !finite(q.ask) || !finite(q.mid)) return { bid: null, mid: null, ask: null }
    const s = sign(leg.side)
    ask += s * leg.ratio * (leg.side === "buy" ? q.ask : q.bid)
    bid += s * leg.ratio * (leg.side === "buy" ? q.bid : q.ask)
    mid += s * leg.ratio * q.mid
  }
  const round = (x: number) => Math.round(x * 100) / 100
  return { bid: round(bid), mid: round(mid), ask: round(ask) }
}

/** Nearest net price on the combo tick, keeping its sign; zero stays even. */
export function roundNet(value: number, tickCents: number): number {
  return (Math.round((value * 100) / tickCents) * tickCents) / 100
}

/** Common names for two- to four-leg strategies; "Custom" otherwise. */
export function strategyLabel(legs: StrategyLeg[]): string {
  const sorted = [...legs].sort((a, b) => a.strike - b.strike)
  const one = legs.every((l) => l.ratio === 1)
  const expiries = new Set(legs.map((l) => l.expiry)).size
  if (legs.length === 1) return `${legs[0]!.side === "buy" ? "Long" : "Short"} ${legs[0]!.type}`
  if (legs.length === 2) {
    const [a, b] = sorted as [StrategyLeg, StrategyLeg]
    if (a.type === b.type && a.side !== b.side && one) {
      if (expiries > 1) return a.strike === b.strike ? "Calendar spread" : "Diagonal spread"
      const title = a.type === "call" ? "call" : "put"
      // A vertical is bullish when it buys the lower strike's call or sells the higher strike's put.
      const bull = a.type === "call" ? a.side === "buy" : b.side === "sell"
      return `${bull ? "Bull" : "Bear"} ${title} spread`
    }
    if (a.type !== b.type && a.side === b.side && one && expiries === 1)
      return `${a.side === "buy" ? "Long" : "Short"} ${a.strike === b.strike ? "straddle" : "strangle"}`
    if (a.type !== b.type && a.side !== b.side && one && expiries === 1) return "Risk reversal"
  }
  if (legs.length === 3 && expiries === 1 && sorted.every((l) => l.type === sorted[0]!.type)) {
    const [low, middle, high] = sorted as [StrategyLeg, StrategyLeg, StrategyLeg]
    if (low.side === high.side && middle.side !== low.side && low.ratio === 1 && high.ratio === 1 && middle.ratio === 2 &&
        middle.strike - low.strike === high.strike - middle.strike)
      return `${low.side === "buy" ? "Long" : "Short"} ${low.type} butterfly`
  }
  if (legs.length === 4 && expiries === 1 && one) {
    const puts = sorted.filter((l) => l.type === "put")
    const calls = sorted.filter((l) => l.type === "call")
    if (puts.length === 2 && calls.length === 2 && puts[0]!.side !== puts[1]!.side && calls[0]!.side !== calls[1]!.side &&
        puts[1]!.side === calls[0]!.side && puts[1]!.strike <= calls[0]!.strike) {
      const short = puts[1]!.side === "sell"
      const butterfly = puts[1]!.strike === calls[0]!.strike
      return `${short ? "" : "Reverse "}iron ${butterfly ? "butterfly" : "condor"}`.replace(/^i/, "I")
    }
  }
  return `Custom · ${legs.length} legs`
}

/** Expiry value of one contract at a spot, times the multiplier. */
function intrinsic(leg: StrategyLeg, spot: number) {
  return 100 * Math.max(0, leg.type === "call" ? spot - leg.strike : leg.strike - spot)
}
/** P&L at expiry of `units` bought at a net debit per unit (negative for a credit). */
export function payoff(legs: StrategyLeg[], units: number, net: number, spot: number): number {
  return legs.reduce((total, leg) => total + sign(leg.side) * leg.ratio * units * intrinsic(leg, spot), 0) - net * 100 * units
}

export interface RiskProfile {
  /** Null when unlimited. */
  maxProfit: number | null
  maxLoss: number | null
  breakevens: number[]
}
/** Max profit, max loss and breakevens at expiry; null when the legs expire apart. */
export function riskProfile(legs: StrategyLeg[], units: number, net: number): RiskProfile | null {
  if (!legs.length || new Set(legs.map((l) => l.expiry)).size > 1 || !(units > 0) || !Number.isFinite(net)) return null
  const points = [0, ...new Set(legs.map((l) => l.strike))].sort((a, b) => a - b)
  const values = points.map((s) => payoff(legs, units, net, s))
  // Beyond the highest strike only calls move: per $1 of spot.
  const slope = legs.reduce((total, leg) => total + (leg.type === "call" ? sign(leg.side) * leg.ratio * units * 100 : 0), 0)
  const high = Math.max(...values), low = Math.min(...values)
  const breakevens: number[] = []
  for (let i = 1; i < points.length; i++) {
    const [a, b] = [values[i - 1]!, values[i]!]
    if (a === 0 && i === 1) breakevens.push(points[0]!)
    if (b === 0) breakevens.push(points[i]!)
    else if ((a < 0 && b > 0) || (a > 0 && b < 0)) breakevens.push(points[i - 1]! + (points[i]! - points[i - 1]!) * (-a / (b - a)))
  }
  const last = values[values.length - 1]!
  if (slope !== 0 && Math.sign(last) === -Math.sign(slope)) breakevens.push(points[points.length - 1]! - last / slope)
  return {
    maxProfit: slope > 0 ? null : Math.max(0, high),
    maxLoss: slope < 0 ? null : Math.max(0, -low),
    breakevens: breakevens.map((b) => Math.round(b * 100) / 100),
  }
}

/**
 * The server's margin rule for these legs alone: shorts paired with same-type
 * longs as verticals (width, never more than naked) and the rest naked at their
 * buy-back value plus the naked requirement; or the worst expiry loss when it
 * is bounded, whichever is less.
 */
export function strategyRequirement(legs: StrategyLeg[], units: number, spot: number | null | undefined): number {
  const naked = (leg: StrategyLeg, n: number) => n * (100 * (leg.quote?.mid ?? 0) + nakedRequirement(leg.type, leg.strike, spot))
  let verticals = 0
  for (const type of ["put", "call"] as const) {
    const order = (a: StrategyLeg, b: StrategyLeg) => (type === "put" ? b.strike - a.strike : a.strike - b.strike)
    const shorts = legs.filter((l) => l.type === type && l.side === "sell").sort(order).map((leg) => ({ leg, left: leg.ratio * units }))
    const longs = legs.filter((l) => l.type === type && l.side === "buy").sort(order).map((leg) => ({ leg, left: leg.ratio * units }))
    let next = 0
    for (const short of shorts) {
      while (short.left > 0 && next < longs.length) {
        const long = longs[next]!
        const n = Math.min(short.left, long.left)
        const width = Math.max(0, type === "put" ? short.leg.strike - long.leg.strike : long.leg.strike - short.leg.strike)
        verticals += Math.min(width * 100 * n, naked(short.leg, n))
        short.left -= n
        if ((long.left -= n) === 0) next++
      }
      if (short.left > 0) verticals += naked(short.leg, short.left)
    }
  }
  const expiries = new Set(legs.map((l) => l.expiry)).size
  const calls = legs.reduce((total, leg) => total + (leg.type === "call" ? sign(leg.side) * leg.ratio : 0), 0)
  if (expiries > 1 || calls < 0) return verticals
  const worst = Math.min(0, ...[0, ...legs.map((l) => l.strike)].map((s) => payoff(legs, units, 0, s)))
  return Math.min(verticals, Math.max(0, -worst))
}

/** Buying power the order would reserve, negative: fees plus its net debit (less a credit) and the legs' requirement. */
export function strategyBuyingPowerEffect(legs: StrategyLeg[], units: number, net: number | null, fee: number, spot: number | null | undefined): number | null {
  if (!legs.length || !Number.isSafeInteger(units) || units <= 0 || net == null || !Number.isFinite(net) || !Number.isFinite(fee)) return null
  const fees = fee * units * legs.reduce((total, leg) => total + leg.ratio, 0)
  return -(fees + Math.max(0, strategyRequirement(legs, units, spot) + net * 100 * units))
}
