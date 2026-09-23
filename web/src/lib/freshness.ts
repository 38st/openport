import type { MarketSession, TradingSession, Num } from "../api/types"

const snapshotDate = new Intl.DateTimeFormat("en-US", {
  timeZone: "America/New_York",
  weekday: "short",
  year: "numeric",
  month: "short",
  day: "numeric",
  hour: "2-digit",
  minute: "2-digit",
  hourCycle: "h23",
})

export function timestampET(iso: string | null | undefined): string {
  const timestamp = iso ? Date.parse(iso) : NaN
  return Number.isFinite(timestamp) ? `${snapshotDate.format(timestamp)} ET` : "—"
}

export function marketBadge(market: MarketSession | TradingSession | null | undefined, stale: boolean) {
  if (market?.open === false) {
    const nextOpen = timestampET("next_open" in market ? market.next_open : null)
    return {
      label: "market closed",
      tone: "neutral" as const,
      title: [market.note, nextOpen === "—" ? null : `Next open: ${nextOpen}`].filter(Boolean).join(" · "),
    }
  }
  if (stale && market?.open === true) return {
    label: "stale",
    tone: "warn" as const,
    title: "Data is older than the provider's stated delay plus 10 minutes",
  }
  if (market?.open && "name" in market && (market.name === "global" || market.name === "curb")) {
    return { label: market.name === "global" ? "overnight session" : "curb session",
      tone: "neutral" as const, title: market.note }
  }
  return null
}

/** Age uses wall time, not the provider's delayed market clock. */
export function snapshotFreshness(asOf: string | null | undefined, delaySeconds: Num = 0, now = Date.now()) {
  const timestamp = asOf ? Date.parse(asOf) : NaN
  if (!Number.isFinite(timestamp) || !Number.isFinite(now)) {
    return { label: "date / age unavailable", ageSeconds: null, stale: false }
  }
  const ageSeconds = Math.max(0, (now - timestamp) / 1000)
  const minutes = Math.floor(ageSeconds / 60)
  const hours = Math.floor(minutes / 60)
  const days = Math.floor(hours / 24)
  const age = days > 0 ? `${days}d ${hours % 24}h old`
    : hours > 0 ? `${hours}h ${minutes % 60}m old`
    : minutes > 0 ? `${minutes} min old` : "<1 min old"
  const delay = delaySeconds != null && Number.isFinite(delaySeconds) ? Math.max(0, delaySeconds) : 0
  return {
    label: `${snapshotDate.format(timestamp)} ET, ${age}`,
    ageSeconds,
    stale: ageSeconds > delay + 10 * 60,
  }
}
