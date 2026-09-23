import { black76, normCdf, type StrategyLeg } from "./strategy"

/** Where a price is expected to be at an expiry: its forward, the time left and the smile's volatility at each level. */
export interface Distribution {
  forward: number
  years: number
  vol: (level: number) => number | null
}

/**
 * Risk-neutral probability that the underlying ends above `level` at expiry, lognormal
 * around the forward: N(d2), d2 = (ln(F/K) − σ²T/2) / (σ√T). Null without a usable volatility.
 */
export function probabilityAbove(level: number, forward: number, years: number, vol: number | null): number | null {
  if (level <= 0) return 1
  if (!(forward > 0) || !(years > 0) || vol == null || !(vol > 0)) return null
  const spread = vol * Math.sqrt(years)
  return normCdf((Math.log(forward / level) - (spread * spread) / 2) / spread)
}

/**
 * Probability of profit at expiry: the risk-neutral mass of the prices where `value`
 * is positive, split at the breakevens. Each breakeven takes the smile's volatility at
 * that level, the usual skew-aware approximation of a digital option.
 */
export function probabilityOfProfit(value: (spot: number) => number, breakevens: readonly number[], d: Distribution): number | null {
  const edges = [...new Set(breakevens.filter((b) => b > 0 && Number.isFinite(b)))].sort((a, b) => a - b)
  const bounds = [0, ...edges, Infinity]
  const above = (level: number) => (level === Infinity ? 0 : probabilityAbove(level, d.forward, d.years, d.vol(level)))
  let total = 0
  for (let i = 0; i + 1 < bounds.length; i++) {
    const [low, high] = [bounds[i]!, bounds[i + 1]!]
    const probe = high === Infinity ? (low > 0 ? low * 1.25 : d.forward) : (low + high) / 2
    if (!(value(probe) > 0)) continue
    const [from, to] = [above(low), above(high)]
    if (from == null || to == null) return null
    total += Math.max(0, from - to)
  }
  return Math.min(1, Math.max(0, total))
}

/** Implied volatility at any level from a chain's smile: linear between strikes, flat beyond the ends. */
export function smileVol(rows: readonly { strike: number; iv: number | null }[], fallback: number | null = null): (level: number) => number | null {
  const points = rows.filter((r) => r.iv != null && Number.isFinite(r.iv) && r.iv > 0)
    .map((r) => [r.strike, r.iv!] as const).sort((a, b) => a[0] - b[0])
  return (level) => {
    if (!points.length) return fallback
    if (level <= points[0]![0]) return points[0]![1]
    const last = points[points.length - 1]!
    if (level >= last[0]) return last[1]
    let lo = 0, hi = points.length - 1
    while (hi - lo > 1) {
      const middle = (lo + hi) >> 1
      if (points[middle]![0] <= level) lo = middle
      else hi = middle
    }
    const [[k0, v0], [k1, v1]] = [points[lo]!, points[hi]!]
    return v0 + ((v1 - v0) * (level - k0)) / (k1 - k0)
  }
}

/** Each leg's expiry as the model needs it: forward, discount factor and years left. */
export interface LegTerms { forward: number | null; discount: number | null; years: number | null }

/**
 * The strategy's P&L now, as a function of spot: every leg at Black-76 with its own
 * implied volatility and time left, its forward carried from today's `spot` by the
 * ratio of its forward to it. Null while a leg lacks a volatility or terms.
 */
export function valueToday(legs: readonly StrategyLeg[], units: number, net: number, spot: number,
  terms: ReadonlyMap<string, LegTerms>): ((spot: number) => number) | null {
  if (!(spot > 0)) return null
  const priced = legs.map((leg) => {
    const t = terms.get(leg.expiry)
    const vol = leg.quote?.iv
    if (!t || t.forward == null || !(t.forward > 0) || t.years == null || !(t.years > 0) || vol == null || !(vol > 0)) return null
    return { leg, carry: t.forward / spot, discount: t.discount ?? 1, years: t.years, vol }
  })
  if (priced.some((p) => p == null)) return null
  return (s) => priced.reduce((total, p) => total + (p!.leg.side === "buy" ? 1 : -1) * p!.leg.ratio * units * 100 *
    black76(p!.leg.type, s * p!.carry, p!.leg.strike, p!.vol, p!.years, p!.discount), 0) - net * 100 * units
}

/** A single contract's breakeven at expiry and its payoff there per contract, before fees. */
export function singleLeg(type: "call" | "put", side: "buy" | "sell", strike: number, premium: number) {
  const breakeven = type === "call" ? strike + premium : strike - premium
  const direction = side === "buy" ? 1 : -1
  const value = (spot: number) => direction * (Math.max(0, type === "call" ? spot - strike : strike - spot) - premium)
  return { breakeven, value }
}
