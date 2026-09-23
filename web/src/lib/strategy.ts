import type { OptionQuote } from "../api/types"
import type { Side } from "../api/trading-types"
import { powerUse, trade, type MarginPosition, type PowerUse } from "./margin"

export type Kind = "call" | "put"
/** A leg as the ticket holds it, with its latest quote. */
export interface StrategyLeg {
  symbol: string
  underlying: string
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
  /** Legs expire apart: values at the first expiry are estimates. */
  estimated?: boolean
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

/** Standard normal CDF from Abramowitz and Stegun's erf approximation 7.1.26 (error under 1.5e-7). */
export function normCdf(x: number): number {
  const t = 1 / (1 + (0.3275911 * Math.abs(x)) / Math.SQRT2)
  const tail = ((((1.061405429 * t - 1.453152027) * t + 1.421413741) * t - 0.284496736) * t + 0.254829592) * t * Math.exp(-(x * x) / 2)
  return x >= 0 ? 1 - tail / 2 : tail / 2
}
/** Black-76 value per unit; intrinsic (discounted) without volatility or time. */
export function black76(type: Kind, forward: number, strike: number, vol: number, years: number, discount: number): number {
  if (!(vol > 0) || !(years > 0)) return discount * Math.max(0, type === "call" ? forward - strike : strike - forward)
  const s = vol * Math.sqrt(years)
  const d1 = (Math.log(forward / strike) + (s * s) / 2) / s, d2 = d1 - s
  return type === "call" ? discount * (forward * normCdf(d1) - strike * normCdf(d2)) : discount * (strike * normCdf(-d2) - forward * normCdf(-d1))
}
/** What the payoff needs to know about an expiry, from the chain. */
export interface ExpiryTerms { id: string; time: number; forward: number | null; discount: number | null }
const YEAR_MS = 365.25 * 86_400_000
/**
 * P&L at the first expiry for `units`, as a function of spot then: legs that
 * expire first at intrinsic value, later legs at Black-76 with their current
 * implied volatility, the forward carried from spot by the ratio of today's
 * forwards and discounted by the ratio of discount factors. Exact when all legs
 * expire together; null when a later leg has no volatility or terms.
 */
export function strategyPayoff(legs: StrategyLeg[], units: number, net: number, terms: Map<string, ExpiryTerms>): ((spot: number) => number) | null {
  const ids = [...new Set(legs.map((l) => l.expiry))]
  if (ids.length === 1) return (spot) => payoff(legs, units, net, spot)
  const byTime = ids.map((id) => terms.get(id)).filter((t): t is ExpiryTerms => t != null).sort((a, b) => a.time - b.time)
  if (byTime.length !== ids.length) return null
  const front = byTime[0]!
  const later = legs.filter((l) => l.expiry !== front.id).map((leg) => {
    const t = terms.get(leg.expiry)!
    const carry = front.forward && t.forward ? t.forward / front.forward : 1
    const discount = front.discount && t.discount ? t.discount / front.discount : 1
    return { leg, carry, discount, years: (t.time - front.time) / YEAR_MS, vol: leg.quote?.iv ?? null }
  })
  if (later.some((l) => l.vol == null || !Number.isFinite(l.vol))) return null
  const first = legs.filter((l) => l.expiry === front.id)
  return (spot) => payoff(first, units, 0, spot) - net * 100 * units + later.reduce((total, { leg, carry, discount, years, vol }) =>
    total + sign(leg.side) * leg.ratio * units * 100 * black76(leg.type, spot * carry, leg.strike, vol!, years, discount), 0)
}
/** Max profit, loss and breakevens sampled from a payoff over a spot range and at the strikes; unbounded as with riskProfile. */
export function estimatedProfile(value: (spot: number) => number, legs: StrategyLeg[], units: number, low: number, high: number): RiskProfile {
  // Near legs' payoffs kink at their strikes, so sample those too.
  const grid = Array.from({ length: 401 }, (_, i) => low + ((high - low) * i) / 400)
  const points = [...new Set([...grid, ...legs.map((l) => l.strike).filter((k) => k > low && k < high)])].sort((a, b) => a - b)
  const values = points.map(value)
  const slope = legs.reduce((total, leg) => total + (leg.type === "call" ? sign(leg.side) * leg.ratio * units : 0), 0)
  const breakevens: number[] = []
  for (let i = 1; i < points.length; i++) {
    const [a, b] = [values[i - 1]!, values[i]!]
    if ((a < 0 && b >= 0) || (a > 0 && b <= 0)) breakevens.push(points[i - 1]! + (points[i]! - points[i - 1]!) * (-a / (b - a)))
  }
  return {
    maxProfit: slope > 0 ? null : Math.max(0, ...values),
    maxLoss: slope < 0 ? null : Math.max(0, -Math.min(...values)),
    breakevens: breakevens.map((b) => Math.round(b * 100) / 100),
    estimated: true,
  }
}

/**
 * Mirrors the server's reservation for a multi-leg order: fees, plus the change
 * in the margin requirement from the held positions to after the fill (legs
 * sold short at their mid), plus the net debit (less a net credit).
 */
export function strategyPowerUse(legs: StrategyLeg[], units: number, net: number | null, fee: number,
  spot: number | null | undefined, held: MarginPosition[] = []): PowerUse | null {
  if (!legs.length || !Number.isSafeInteger(units) || units <= 0 || net == null || !Number.isFinite(net) || !Number.isFinite(fee)) return null
  const fees = fee * units * legs.reduce((total, leg) => total + leg.ratio, 0)
  const after = legs.reduce((book, leg) => trade(book, leg, sign(leg.side) * leg.ratio * units, leg.quote?.mid ?? 0), held)
  return powerUse(held, after, net * 100 * units, fees, spot)
}
