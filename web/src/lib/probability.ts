import type { Expiry, Surface } from "../api/types"
import { black76, normCdf, type StrategyLeg } from "./strategy"
import { ssviVol, sviVol } from "./svi"

/** A survival CDF is required so profit calculations cannot silently drop the smile skew. */
export interface Distribution {
  forward: number
  years: number
  vol: (level: number) => number | null
  above: (level: number) => number | null
}

export interface SmileDistribution extends Distribution {
  source: "svi" | "ssvi" | "chain" | "lognormal"
  /** Derivative of decimal volatility with respect to ln(K/F). */
  slope: (level: number) => number | null
  repaired: boolean
  range: readonly [number, number] | null
}

type SmileRow = { strike: number; iv: number | null }
const positive = (x: number | null | undefined): x is number => x != null && Number.isFinite(x) && x > 0
const clamp = (x: number) => Math.max(0, Math.min(1, x))

/** A digital from the smile's call-price derivative; a flat smile reduces to N(d2). */
export function probabilityAbove(level: number, forward: number, years: number, vol: number | null, slope = 0): number | null {
  if (level <= 0) return 1
  if (level === Infinity) return 0
  if (!positive(forward) || !positive(years) || !positive(vol) || !Number.isFinite(level) || !Number.isFinite(slope)) return null
  const spread = vol * Math.sqrt(years)
  const d2 = (Math.log(forward / level) - spread * spread / 2) / spread
  return normCdf(d2) - Math.exp(-d2 * d2 / 2) / Math.sqrt(2 * Math.PI) * Math.sqrt(years) * slope
}

/** Risk-neutral mass in the profitable intervals, split at the payoff's breakevens. */
export function probabilityOfProfit(value: (spot: number) => number, breakevens: readonly number[], d: Distribution): number | null {
  const edges = [...new Set(breakevens.filter((b) => b > 0 && Number.isFinite(b)))].sort((a, b) => a - b)
  const bounds = [0, ...edges, Infinity]
  const above = (level: number) => level <= 0 ? 1 : level === Infinity ? 0
    : d.above(level)
  let total = 0
  for (let i = 0; i + 1 < bounds.length; i++) {
    const [low, high] = [bounds[i]!, bounds[i + 1]!]
    const probe = high === Infinity ? (low > 0 ? low * 1.25 : d.forward) : (low + high) / 2
    if (!(value(probe) > 0)) continue
    const [from, to] = [above(low), above(high)]
    if (from == null || to == null) return null
    total += Math.max(0, from - to)
  }
  return clamp(total)
}

function smilePoints(rows: readonly SmileRow[]) {
  const grouped = new Map<number, number[]>()
  for (const row of rows) if (positive(row.strike) && positive(row.iv)) {
    const values = grouped.get(row.strike) ?? []
    values.push(row.iv)
    grouped.set(row.strike, values)
  }
  return [...grouped].sort((a, b) => a[0] - b[0]).map(([strike, values]) => ({
    x: Math.log(strike), y: values.reduce((sum, v) => sum + v, 0) / values.length,
  }))
}

function interpolate(points: readonly { x: number; y: number }[], x: number): number {
  if (x <= points[0]!.x) return points[0]!.y
  if (x >= points.at(-1)!.x) return points.at(-1)!.y
  let lo = 0, hi = points.length - 1
  while (hi - lo > 1) {
    const middle = (lo + hi) >> 1
    if (points[middle]!.x <= x) lo = middle
    else hi = middle
  }
  const a = points[lo]!, b = points[hi]!
  return a.y + (b.y - a.y) * (x - a.x) / (b.x - a.x)
}

/** Chain IV interpolated in log strike (equivalently log moneyness), flat beyond quotes. */
export function smileVol(rows: readonly SmileRow[], fallback: number | null = null): (level: number) => number | null {
  const points = smilePoints(rows)
  return (level) => points.length ? interpolate(points, Math.log(level)) : fallback
}

/** Use the expiry's checked raw SVI, then SSVI, then its quoted smile, then a flat IV. */
export function smileDistribution(expiry: Pick<Expiry, "id" | "forward" | "days" | "atm_iv">,
  rows: readonly SmileRow[] = [], surface?: Surface, fallback: number | null = expiry.atm_iv): SmileDistribution | null {
  const fitted = surface?.expiries.find((e) => e.id === expiry.id)
  const points = smilePoints(rows)
  const fitForward = fitted?.forward
  const chainYears = (expiry.days ?? 0) / 365
  const fitTime = fitted?.svi_years ?? (fitted?.days != null ? fitted.days / 365 : chainYears)
  const forward = positive(expiry.forward) ? expiry.forward : fitForward
  const years = positive(chainYears) ? chainYears : fitTime
  if (!positive(forward) || !positive(years)) return null
  let time = years
  let modelForward = forward
  let source: SmileDistribution["source"] = points.length >= 2 ? "chain" : "lognormal"
  let range: [number, number] | null = points.length >= 2 ? [Math.exp(points[0]!.x), Math.exp(points.at(-1)!.x)] : null
  let vol = smileVol(rows, positive(fallback) ? fallback : null)
  // Five neighbouring strikes give a local least-squares skew, instead of noisy one-sided digitals.
  const slopes = points.map((p, i) => {
    const start = Math.max(0, Math.min(i - 2, points.length - 5))
    const neighbours = points.slice(start, start + 5)
    const x = neighbours.reduce((sum, n) => sum + n.x, 0) / neighbours.length
    const y = neighbours.reduce((sum, n) => sum + n.y, 0) / neighbours.length
    const variance = neighbours.reduce((sum, n) => sum + (n.x - x) ** 2, 0)
    return { x: p.x, y: variance > 0 && neighbours.some((n) => n.y !== p.y) ? neighbours.reduce((sum, n) => sum + (n.x - x) * (n.y - y), 0) / variance : 0 }
  })
  let slope = (level: number): number | null => range && level >= range[0] && level <= range[1]
    ? interpolate(slopes, Math.log(level)) : 0
  const useFit = (kind: "svi" | "ssvi", min: number | null | undefined, max: number | null | undefined,
    evaluate: (k: number) => number | null, derivative: (k: number, iv: number) => number) => {
    if (!positive(fitForward) || !positive(fitTime)) return false
    const ks = fitted?.points.map((p) => p.k).filter((k): k is number => k != null && Number.isFinite(k)) ?? []
    const low = min ?? Math.min(...ks), high = max ?? Math.max(...ks)
    if (!Number.isFinite(low) || !Number.isFinite(high) || !(high > low)) return false
    for (let i = 0; i <= 64; i++) {
      const k = low + (high - low) * i / 64, iv = evaluate(k)
      if (!positive(iv) || !Number.isFinite(derivative(k, iv))) return false
    }
    source = kind
    modelForward = fitForward
    time = fitTime
    range = [fitForward * Math.exp(low), fitForward * Math.exp(high)]
    vol = (level) => evaluate(Math.max(low, Math.min(high, Math.log(level / fitForward))))
    slope = (level) => {
      if (level < range![0] || level > range![1]) return 0
      const k = Math.log(level / fitForward), iv = evaluate(k)
      return positive(iv) ? derivative(k, iv) : null
    }
    return true
  }
  const p = fitted?.svi
  const calendarOk = !surface?.calendar_violations?.some((v) => v.earlier === expiry.id || v.later === expiry.id)
  const raw = p?.status === "ok" && p.butterfly_ok && calendarOk &&
    useFit("svi", fitted?.svi_min_k, fitted?.svi_max_k, (k) => sviVol(p, k, fitTime),
      (k, iv) => p.b * (p.rho + (k - p.m) / Math.hypot(k - p.m, p.sigma)) / (2 * fitTime * iv))
  const ssvi = surface?.ssvi, theta = fitted?.ssvi_theta
  if (!raw && ssvi && positive(theta)) {
    const phi = ssvi.eta! / (theta ** ssvi.gamma! * (1 + theta) ** (1 - ssvi.gamma!))
    useFit("ssvi", fitted?.ssvi_min_k, fitted?.ssvi_max_k, (k) => ssviVol(ssvi, k, theta, fitTime),
      (k, iv) => theta * phi / 2 * (ssvi.rho! + (phi * k + ssvi.rho!) /
        Math.hypot(phi * k + ssvi.rho!, Math.sqrt(1 - ssvi.rho! ** 2))) / (2 * fitTime * iv))
  }
  const digital = (level: number) => probabilityAbove(level, modelForward, time, vol(level), slope(level) ?? NaN)
  const result: SmileDistribution = { forward: modelForward, years: time, vol, slope, source, range, repaired: false, above: digital }
  if (!range || !positive(vol(range[0])) || !positive(vol(range[1]))) return result
  const lo = Math.log(range[0]), hi = Math.log(range[1])
  // Include both sides of the flat-wing joins and every quoted knot. The same grid is
  // used for every query, so repair is independent of breakevens and query order.
  const xs = [...new Set([lo - 1e-10, hi + 1e-10, ...points.map((p) => p.x).filter((x) => x > lo && x < hi),
    ...Array.from({ length: 4097 }, (_, i) => lo + (hi - lo) * i / 4096)])].sort((a, b) => a - b)
  let minimum = 1
  const grid = xs.map((x) => {
    const rawValue = digital(Math.exp(x))
    if (rawValue == null) return { x, y: NaN }
    const y = Math.min(minimum, clamp(rawValue))
    if (Math.abs(y - rawValue) > 1e-10) result.repaired = true
    minimum = y
    return { x, y }
  })
  if (grid.some((p) => !Number.isFinite(p.y))) return { ...result, above: () => null }
  // Preserve the exact lognormal answer for flat smiles, including at unlisted strikes.
  if (xs.every((x) => slope(Math.exp(x)) === 0 && vol(Math.exp(x)) === vol(range![0]))) return result
  result.above = (level) => {
    if (level <= 0) return 1
    if (level === Infinity) return 0
    if (!Number.isFinite(level)) return null
    const x = Math.log(level)
    if (x < grid[0]!.x) return Math.max(grid[0]!.y, clamp(digital(level)!))
    if (x > grid.at(-1)!.x) return Math.min(grid.at(-1)!.y, clamp(digital(level)!))
    return interpolate(grid, x)
  }
  return result
}

export function probabilitySource(d: SmileDistribution, levels: readonly number[]): string {
  const source = d.source === "chain" ? "from the chain's smile" : d.source === "lognormal" ? "lognormal, no smile" : "from the fitted smile"
  const outside = d.range && levels.some((k) => k > 0 && (k < d.range![0] || k > d.range![1]))
  return [source, d.repaired ? "approximate: monotonicity repaired" : null, outside ? "volatility held flat beyond the smile" : null].filter(Boolean).join(" · ")
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

/** "0, 1, 7" as distinct numbers within [minimum, maximum], at most `most`; null when any entry does not parse. */
export function parseNumbers(text: string, most: number, minimum: number, maximum: number): number[] | null {
  const values: number[] = []
  for (const part of text.split(",").map((p) => p.trim()).filter(Boolean)) {
    if (!/^\d+(\.\d+)?$/.test(part)) return null
    const value = Number(part)
    if (!(value >= minimum && value <= maximum)) return null
    if (!values.includes(value)) values.push(value)
  }
  return values.length <= most ? values : null
}
