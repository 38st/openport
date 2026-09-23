import type { OptionQuote } from "../api/types"
import type { Side } from "../api/trading-types"
import { price as formatPrice } from "./format"

type Kind = "call" | "put"
const title = (type: Kind) => (type === "call" ? "Call" : "Put")

/** Closing and opening quantities of an order against the held position. */
export function split(side: Side, quantity: number, held: number) {
  const closing = Math.max(0, Math.min(quantity, side === "buy" ? -held : held))
  return { closing, opening: Math.max(0, quantity - closing) }
}

/** "Long Call", "Close Short Put", "Reverse to Short Call"... */
export function strategyName(side: Side, type: Kind, held: number, quantity: number): string {
  const { closing, opening } = split(side, quantity, held)
  const direction = side === "buy" ? "Long" : "Short"
  if (closing === 0) return `${direction} ${title(type)}`
  if (opening === 0) return `Close ${side === "buy" ? "Short" : "Long"} ${title(type)}`
  return `Reverse to ${direction} ${title(type)}`
}

export interface Marketability {
  marketable: boolean
  /** The executable far side, when there is one. */
  price: number | null
  size: number | null
  message: string
}
/** Mirrors the matcher: buys execute at the ask when ask <= limit, sells at the bid when bid >= limit. */
export function marketability(side: Side, type: "limit" | "market", limit: string, quote: OptionQuote | null): Marketability {
  const buy = side === "buy"
  const far = buy ? quote?.ask : quote?.bid
  const size = buy ? quote?.ask_size : quote?.bid_size
  const book = buy ? "ask" : "bid"
  if (far == null || !Number.isFinite(far) || far <= 0)
    return { marketable: false, price: null, size: null, message: `No executable ${book}; the order cannot fill until one is quoted.` }
  const depth = size != null && Number.isFinite(size) ? `, up to ${size} displayed` : ""
  if (type === "market")
    return { marketable: true, price: far, size: size ?? null, message: `Fills now at the ${book} $${formatPrice(far)}${depth}; any remainder cancels.` }
  const value = Number(limit)
  if (!limit || !Number.isFinite(value) || value <= 0)
    return { marketable: false, price: far, size: size ?? null, message: "Enter a limit price." }
  if (buy ? far <= value : far >= value)
    return { marketable: true, price: far, size: size ?? null, message: `Marketable: fills now at the ${book} $${formatPrice(far)}${depth}.` }
  return { marketable: false, price: far, size: size ?? null,
    message: `Rests ${buy ? "below the ask" : "above the bid"} ($${formatPrice(far)}). Fills when the ${book} reaches $${formatPrice(value)}.` }
}

/** Per-contract naked short requirement, excluding premium; the strike stands in for a missing spot. */
export function nakedRequirement(type: Kind, strike: number, spot: number | null | undefined): number {
  const s = spot != null && Number.isFinite(spot) && spot > 0 ? spot : strike
  const otm = Math.max(0, type === "call" ? strike - s : s - strike)
  return 100 * Math.max(0.2 * s - otm, 0.1 * (type === "call" ? s : strike))
}

export type Direction = "at_or_below" | "at_or_above"
export const opposite = (direction: Direction): Direction => (direction === "at_or_below" ? "at_or_above" : "at_or_below")
/**
 * Where a stop sits for an entry. Option-price stops watch the exit's side
 * (the bid when selling out of a long), so they trigger on the way down for
 * longs and up for shorts. Underlying stops trigger when spot moves against the
 * position: down for long calls and short puts, up for long puts and short calls.
 */
export function stopDirection(source: "option" | "underlying", entry: Side, type: Kind): Direction {
  if (source === "option") return entry === "buy" ? "at_or_below" : "at_or_above"
  return (entry === "buy") === (type === "call") ? "at_or_below" : "at_or_above"
}
/** A conditional entry fires when spot reaches the level from where it is now. */
export function crossDirection(level: number, spot: number | null | undefined): Direction {
  return spot != null && Number.isFinite(spot) && level < spot ? "at_or_below" : "at_or_above"
}
/** "bid ≤ $3.50" or "SPX ≥ 5,010.00". */
export function describeTrigger(trigger: { source: "option" | "underlying"; direction: Direction; level: string }, side: Side, underlying: string): string {
  const sign = trigger.direction === "at_or_below" ? "≤" : "≥"
  const level = Number(trigger.level)
  const text = Number.isFinite(level) ? level.toLocaleString("en-US", { minimumFractionDigits: 2, maximumFractionDigits: 2 }) : trigger.level
  return trigger.source === "option" ? `${side === "buy" ? "ask" : "bid"} ${sign} $${text}` : `${underlying} ${sign} ${text}`
}
