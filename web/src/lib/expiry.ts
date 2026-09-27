import type { Expiry } from "../api/types"

/** The nearest expiry that still trades: its auto-close is ahead on the market clock. Older servers without the time fall back to time left. */
export function defaultExpiry(expiries: Expiry[], now: number): string | null {
  return (expiries.find((e) => e.auto_close && Number.isFinite(now)
    ? Date.parse(e.auto_close) > now : (e.days ?? 0) > 0) ?? expiries[0])?.id ?? null
}

const marketDate = (time: number) => new Date(time).toLocaleDateString("en-CA", { timeZone: "America/New_York" })
const two = (n: number) => String(n).padStart(2, "0")

/** "auto-close in 12:34", or "1:02:03" from an hour out, on the day of the expiry's auto-close only. */
export function autoCloseCountdown(expiry: Expiry | undefined, now: number): string | null {
  const close = expiry?.auto_close ? Date.parse(expiry.auto_close) : Number.NaN
  if (!Number.isFinite(close) || !Number.isFinite(now) || marketDate(close) !== marketDate(now)) return null
  const seconds = Math.max(0, Math.ceil((close - now) / 1000))
  if (seconds === 0) return "auto-close reached"
  const [hours, minutes] = [Math.floor(seconds / 3600), Math.floor((seconds % 3600) / 60)]
  return `auto-close in ${hours > 0 ? `${hours}:${two(minutes)}` : minutes}:${two(seconds % 60)}`
}
