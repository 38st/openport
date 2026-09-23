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

/**
 * Estimated change in buying power, negative when it uses power, following the
 * server's reservations: opening buys pay premium and fees, opening sells hold
 * the naked requirement plus fees, closing contracts cost only fees.
 */
export function buyingPowerEffect(order: { side: Side; quantity: number; price: number | null; fee: number; held: number;
  type: Kind; strike: number; spot: number | null | undefined }): number | null {
  const { side, quantity, price, fee, held } = order
  if (!Number.isSafeInteger(quantity) || quantity <= 0 || !Number.isFinite(fee)) return null
  const { opening } = split(side, quantity, held)
  if (side === "buy") return price == null || !Number.isFinite(price) ? null : -(opening * 100 * price + fee * quantity)
  return -(opening * nakedRequirement(order.type, order.strike, order.spot) + fee * quantity)
}
