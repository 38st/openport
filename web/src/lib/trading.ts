import type { OptionQuote } from "../api/types"
import type { Money, Side } from "../api/trading-types"

function decimal(value: string | null | undefined) {
  if (value == null || !/^-?\d+(\.\d+)?$/.test(value)) return null
  const negative = value.startsWith("-")
  const [whole = "0", fraction = ""] = value.replace(/^-/, "").split(".")
  return { units: BigInt(whole + fraction) * (negative ? -1n : 1n), scale: fraction.length }
}
function decimalString(units: bigint, scale: number) {
  const negative = units < 0n
  const digits = (negative ? -units : units).toString().padStart(scale + 1, "0")
  return `${negative ? "-" : ""}${scale ? `${digits.slice(0, -scale)}.${digits.slice(-scale)}` : digits}`
}
/** Exact integer multiplication; no binary floating point accounting. */
export function multiplyMoney(value: Money | null | undefined, multiplier: number): Money | null {
  const parsed = decimal(value)
  return parsed && Number.isSafeInteger(multiplier) ? decimalString(parsed.units * BigInt(multiplier), parsed.scale) : null
}
export function validMoney(value: string) {
  const parsed = decimal(value)
  return parsed != null && parsed.units >= 0n
}
/** Round only for display, half away from zero, without losing large integer cents. */
export function formatMoney(value: Money | null | undefined, digits = 2): string {
  const parsed = decimal(value)
  if (!parsed) return "—"
  const absolute = parsed.units < 0n ? -parsed.units : parsed.units
  const factor = 10n ** BigInt(Math.abs(parsed.scale - digits))
  const rounded = parsed.scale > digits ? (absolute + factor / 2n) / factor : absolute * factor
  const [whole = "0", fraction] = decimalString(rounded, digits).split(".")
  return `${parsed.units < 0n && rounded !== 0n ? "−" : ""}$${whole.replace(/\B(?=(\d{3})+(?!\d))/g, ",")}${fraction ? `.${fraction}` : ""}`
}
export const sideFromCell = (cell: "bid" | "ask"): Side => cell === "bid" ? "sell" : "buy"

export function ticketEstimate(quote: OptionQuote | null, side: Side, quantity: number, price: Money | null, fee: Money | null) {
  const valid = Number.isSafeInteger(quantity) && quantity > 0 && Number.isSafeInteger(quantity * 100)
  const multiplier = quantity * 100 * (side === "buy" ? 1 : -1)
  const greek = (key: "delta" | "gamma" | "vega" | "theta") => {
    const value = quote?.[key]
    return valid && value != null && Number.isFinite(value) ? value * multiplier : null
  }
  return {
    premium: valid ? multiplyMoney(price, quantity * 100) : null,
    fees: valid ? multiplyMoney(fee, quantity) : null,
    delta: greek("delta"), gamma: greek("gamma"), vega: greek("vega"), theta: greek("theta"),
  }
}

export function scenarioScale(values: (number | null)[][]) {
  return values.flat().reduce<number>((max, value) => value != null && Number.isFinite(value) ? Math.max(max, Math.abs(value)) : max, 0) || 1
}
export function scenarioColour(value: number | null | undefined, scale: number) {
  if (value == null || !Number.isFinite(value) || value === 0) return "var(--panel)"
  const intensity = Math.min(1, Math.abs(value) / Math.max(scale, Number.EPSILON)) * 30
  return `color-mix(in srgb, var(${value > 0 ? "--bullish" : "--bearish"}) ${intensity}%, var(--panel))`
}
