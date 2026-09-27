import type { Chain, ChainRow, OptionQuote } from "../api/types"
import type { Side } from "../api/trading-types"
import { expectedMove } from "./candles"

export type Kind = "call" | "put"
/** A leg as the ticket holds it, with its latest quote. */
export interface StrategyLeg {
  symbol: string
  underlying: string
  side: Side
  ratio: number
  type: Kind
  strike: number
  /** Expiry id, including settlement family. */
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

const gcd = (a: number, b: number): number => (b === 0 ? a : gcd(b, a % b))
/**
 * Legs that close held positions together: each reversed, sizes reduced to a
 * ratio per unit. Null with a reason when they cannot form one order.
 */
export function closingLegs(positions: { symbol: string; underlying: string; expiry: string; settlement: "AM" | "PM"; type: Kind; strike: number; quantity: number }[]):
  { legs: StrategyLeg[]; units: number } | { reason: string } {
  const held = positions.filter((p) => p.quantity !== 0)
  if (held.length < 2 || held.length > MAX_LEGS) return { reason: `Choose two to ${MAX_LEGS} positions.` }
  if (new Set(held.map((p) => p.underlying)).size > 1) return { reason: "Positions closed together must share one underlying." }
  const units = held.reduce((g, p) => gcd(g, Math.abs(p.quantity)), 0)
  const legs = held.map((p) => ({ symbol: p.symbol, underlying: p.underlying, side: (p.quantity > 0 ? "sell" : "buy") as Side,
    ratio: Math.abs(p.quantity) / units, type: p.type, strike: p.strike, expiry: `${p.expiry}${p.settlement}`, quote: null }))
  if (legs.some((l) => l.ratio > MAX_RATIO)) return { reason: `Sizes this uneven exceed a ${MAX_RATIO}-to-1 ratio; close them separately.` }
  return { legs, units }
}

export type TemplateTarget = { mode: "atm" } | { mode: "delta" | "points" | "moves" | "strike"; value: number }
export type StrategyTemplate =
  | { kind: "vertical"; type: Kind; direction: "credit" | "debit"; target: TemplateTarget; width: number }
  | { kind: "condor"; target: TemplateTarget; width: number }
  | { kind: "iron-butterfly"; width: number }
  | { kind: "strangle"; delta: number; side: Side }
  | { kind: "straddle"; side: Side }
  | { kind: "butterfly"; type: Kind; target: TemplateTarget; width: number }
  | { kind: "calendar" | "diagonal"; type: Kind; target: TemplateTarget; farExpiry: string; offset: number }

export interface TemplateSetup { legs: StrategyLeg[]; tag: string; widths: number[] }
export type TemplateResult = TemplateSetup | { reason: string }

const targetTag = (target: TemplateTarget) => target.mode === "atm" ? "atm"
  : `${target.value}${{ delta: "d", points: "pt", moves: "em", strike: "k" }[target.mode]}`
/** Describes requested parameters; actual snapped strikes and widths remain in the review. */
export function templateTag(template: StrategyTemplate): string {
  return setupTag(template).slice(0, 32)
}
function setupTag(template: StrategyTemplate): string {
  switch (template.kind) {
    case "vertical": return `${template.type}-${template.direction}-${targetTag(template.target)}-${template.width}w`
    case "condor": return `iron-condor-${targetTag(template.target)}-${template.width}w`
    case "iron-butterfly": return `iron-butterfly-atm-${template.width}w`
    case "strangle": return `${template.side === "sell" ? "short" : "long"}-strangle-${template.delta}d`
    case "straddle": return `${template.side === "sell" ? "short" : "long"}-straddle-atm`
    case "butterfly": return `long-${template.type}-butterfly-${targetTag(template.target)}-${template.width}w`
    // The setup, not the dates: a tag groups every trade of this shape in the Journal.
    case "calendar": case "diagonal": return `${template.type}-${template.kind}-${targetTag(template.target)}${template.kind === "diagonal" ? `-${template.offset}pt` : ""}`
  }
}

/** Deterministic leg selection from listed contracts. Never substitute another strike for a missing quote. */
export function buildTemplate(template: StrategyTemplate, near: Chain, far?: Chain): TemplateResult {
  const fail = (reason: string): never => { throw new Error(reason) }
  const rows = near.strikes.filter((r) => finite(r.strike) && r.strike > 0).sort((a, b) => a.strike - b.strike)
  const nearest = (chain: Chain, target: number, purpose: string): ChainRow => {
    const listed = chain.strikes.filter((r) => finite(r.strike) && r.strike > 0).sort((a, b) => a.strike - b.strike)
    if (!listed.length) return fail(`${chain.expiry.id}: no listed strikes for ${purpose}.`)
    if (!finite(target) || target < listed[0]!.strike || target > listed.at(-1)!.strike)
      return fail(`${purpose} at ${target}: outside the loaded strike range ${listed[0]!.strike}–${listed.at(-1)!.strike}. Widen the chain window.`)
    return listed.reduce((best, row) => Math.abs(row.strike - target) < Math.abs(best.strike - target) ? row : best)
  }
  const atm = () => {
    if (!finite(near.expiry.forward) || near.expiry.forward <= 0) return fail("The selected expiry has no forward for an ATM strike.")
    return near.expiry.forward
  }
  const pick = (type: Kind, target: TemplateTarget) => {
    if (target.mode === "atm") return nearest(near, atm(), "ATM strike")
    if (!finite(target.value)) return fail("Enter a finite strike target.")
    if (target.mode === "delta") {
      if (!(target.value > 0 && target.value < 100)) return fail("Target delta must be between 0 and 100, excluding the endpoints.")
      const signed = (type === "put" ? -1 : 1) * target.value / 100
      const candidates = rows.filter((row) => finite(row[type]?.delta) && (type === "put"
        ? row[type]!.delta! >= -1 && row[type]!.delta! <= 0 : row[type]!.delta! >= 0 && row[type]!.delta! <= 1))
      if (!candidates.length) return fail(`No ${type} deltas in the loaded chain.`)
      const deltas = candidates.map((row) => row[type]!.delta!)
      if (signed < Math.min(...deltas) || signed > Math.max(...deltas)) return fail(`${target.value}Δ ${type} is outside the loaded delta range. Widen the chain window.`)
      return candidates.reduce((best, row) => Math.abs(row[type]!.delta! - signed) < Math.abs(best[type]!.delta! - signed) ? row : best)
    }
    if (target.mode === "strike") return nearest(near, target.value, "Centre strike")
    let offset = target.value
    if (target.mode === "moves") {
      const move = expectedMove(near.expiry.forward, near.expiry.atm_iv, near.expiry.days)
      if (move == null || !(move > 0)) return fail("Expected move unavailable: the expiry needs a forward, ATM volatility and time remaining.")
      offset *= move
    }
    return nearest(near, atm() + (type === "put" ? -offset : offset), `${type} target`)
  }
  const leg = (chain: Chain, row: ChainRow, type: Kind, side: Side, ratio = 1): StrategyLeg => {
    const quote = row[type]
    const name = `${chain.expiry.id} ${row.strike} ${type}`
    if (!quote || !finite(quote.bid) || !finite(quote.ask) || !finite(quote.mid) || quote.bid < 0 || quote.ask < quote.bid)
      return fail(`${name}: no valid two-sided quote.`)
    if (!quote.symbol || quote.tradable !== true) return fail(`${name}: ${quote.untradable_reason ?? "unavailable for paper trading"}.`)
    return { symbol: quote.symbol, underlying: chain.symbol, side, ratio, type, strike: row.strike, expiry: chain.expiry.id, quote }
  }
  const wing = (center: ChainRow, offset: number, type: Kind, side: Side) => {
    const row = nearest(near, center.strike + offset, `${type} wing`)
    if ((row.strike - center.strike) * offset <= 0) return fail(`${type} wing snaps to the centre strike ${center.strike}; increase the width.`)
    return leg(near, row, type, side)
  }
  try {
    if (!rows.length) return fail("The selected chain has no listed strikes.")
    if ("width" in template && (!finite(template.width) || template.width <= 0)) return fail("Width must be greater than zero points.")
    let legs: StrategyLeg[]
    let widths: number[] = []
    switch (template.kind) {
      case "vertical": {
        const short = pick(template.type, template.target)
        const direction = (template.type === "call" ? 1 : -1) * (template.direction === "credit" ? 1 : -1)
        const long = wing(short, direction * template.width, template.type, "buy")
        legs = [leg(near, short, template.type, "sell"), long]
        widths = [Math.abs(short.strike - long.strike)]
        break
      }
      case "condor": case "iron-butterfly": {
        const target: TemplateTarget = template.kind === "iron-butterfly" ? { mode: "atm" } : template.target
        if (template.kind === "condor" && target.mode !== "delta" && target.mode !== "moves") return fail("Condor shorts require a delta or expected-move target.")
        if ("value" in target && target.value <= 0) return fail("Condor target must be greater than zero.")
        const put = pick("put", target), call = pick("call", target)
        if (template.kind === "condor" && put.strike >= call.strike) return fail("Condor short put must be below the short call after snapping.")
        const low = wing(put, -template.width, "put", "buy"), high = wing(call, template.width, "call", "buy")
        legs = [low, leg(near, put, "put", "sell"), leg(near, call, "call", "sell"), high]
        widths = [put.strike - low.strike, high.strike - call.strike]
        break
      }
      case "strangle": case "straddle": {
        const target: TemplateTarget = template.kind === "straddle" ? { mode: "atm" } : { mode: "delta", value: template.delta }
        const put = pick("put", target), call = pick("call", target)
        if (template.kind === "strangle" && put.strike >= call.strike) return fail("Strangle put must be below the call after snapping.")
        legs = [leg(near, put, "put", template.side), leg(near, call, "call", template.side)]
        widths = [call.strike - put.strike]
        break
      }
      case "butterfly": {
        if (template.target.mode !== "atm" && template.target.mode !== "strike") return fail("Butterfly centre requires ATM or a strike.")
        const center = pick(template.type, template.target)
        const low = wing(center, -template.width, template.type, "buy"), high = wing(center, template.width, template.type, "buy")
        if (Math.abs((center.strike - low.strike) - (high.strike - center.strike)) > 1e-8) return fail("Listed strikes cannot form equal butterfly wings at this centre and width.")
        legs = [low, leg(near, center, template.type, "sell", 2), high]
        widths = [center.strike - low.strike, high.strike - center.strike]
        break
      }
      case "calendar": case "diagonal": {
        if (template.target.mode !== "atm" && template.target.mode !== "delta") return fail("Calendar and diagonal strikes require ATM or delta.")
        if (!far || far.expiry.id !== template.farExpiry) return fail(`Load the far chain (${template.farExpiry || "choose an expiry"}) before building this template.`)
        if (far.symbol !== near.symbol) return fail("Both expiries must have the same underlying.")
        if (!(Date.parse(far.expiry.expiry_time) > Date.parse(near.expiry.expiry_time))) return fail("Far expiry must settle after the selected expiry.")
        const short = pick(template.type, template.target)
        let long: ChainRow
        if (template.kind === "calendar") long = far.strikes.find((r) => r.strike === short.strike) ?? fail(`Far expiry has no listed ${short.strike} strike for a calendar.`)
        else {
          if (!finite(template.offset) || template.offset === 0) return fail("Diagonal far-strike offset must be nonzero points.")
          long = nearest(far, short.strike + template.offset, "Far strike")
          if (long.strike === short.strike) return fail("Diagonal offset snaps to the near strike; increase its size.")
        }
        legs = [leg(near, short, template.type, "sell"), leg(far, long, template.type, "buy")]
        widths = [Math.abs(long.strike - short.strike)]
        break
      }
    }
    if (new Set(legs.map((l) => l.symbol)).size !== legs.length) return fail("The selected contracts overlap after snapping.")
    return { legs, tag: templateTag(template), widths }
  } catch (error) {
    return { reason: error instanceof Error ? error.message : "Unable to select template legs." }
  }
}
