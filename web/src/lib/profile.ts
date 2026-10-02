import type { Marker, Reference, Series } from "../charts/LineChart"
import type { BetaSource, ProfileCurve, ProfileLevel, RiskProfile } from "../api/trading-types"

const colours = ["var(--chart-1)", "var(--chart-2)", "var(--chart-3)", "var(--chart-4)", "var(--chart-5)", "var(--chart-6)", "var(--chart-7)", "var(--chart-8)"]

/** "Today", "T+1", "T+0.5" or "Expiry (T+30.3)". */
export function curveLabel(curve: Pick<ProfileCurve, "days" | "label">): string {
  const days = Number.isInteger(curve.days) ? String(curve.days) : curve.days.toFixed(1)
  if (curve.label === "expiry") return `Expiry (T+${days})`
  return curve.days === 0 ? "Today" : `T+${days}`
}

/**
 * The curves against the reference's price, or its percent move without one; the
 * floors as the loss that reaches them; the reference's price now and the widest
 * horizon's one standard deviation cone as markers.
 */
export function profileChart(profile: RiskProfile): { series: Series[]; references: Reference[]; markers: Marker[]; byPrice: boolean } {
  const byPrice = profile.spot != null && profile.prices.every((price) => price != null)
  const xs = byPrice ? profile.prices.map(Number) : profile.percent
  const series: Series[] = profile.curves.map((curve, index) => ({
    id: `curve-${index}`, label: curveLabel(curve), color: colours[index % colours.length]!,
    dashed: index > 0, points: xs.map((x, i) => ({ x, y: profile.complete ? curve.pnl[i] ?? null : null })),
  }))
  const references: Reference[] = []
  const room = profile.room != null ? Number(profile.room) : null
  const soft = profile.soft_room != null ? Number(profile.soft_room) : null
  if (room != null && Number.isFinite(room)) references.push({ y: -room, label: "Plan floor", color: "var(--bearish)" })
  if (soft != null && Number.isFinite(soft) && soft !== room) references.push({ y: -soft, label: "Soft floor", color: "var(--warn)" })
  const markers: Marker[] = []
  if (byPrice && profile.spot != null) markers.push({ x: profile.spot, label: `${profile.reference} now`, color: "var(--muted)" })
  const widest = [...profile.curves].reverse().find((curve) => curve.horizon.one_sd != null)
  if (byPrice && widest?.horizon.one_sd) {
    const label = `1σ by ${curveLabel(widest)}`
    const [low, high] = [xs[0]!, xs[xs.length - 1]!]
    for (const x of [widest.horizon.one_sd.low, widest.horizon.one_sd.high])
      if (x >= low && x <= high) markers.push({ x, label, color: "var(--accent)" })
  }
  return { series, references, markers, byPrice }
}

/** A floor level as "4,812.50 (−3.75%) · 18% touch". */
export function levelText(level: ProfileLevel | null): string {
  if (!level) return "—"
  const percent = `${level.percent > 0 ? "+" : level.percent < 0 ? "−" : ""}${Math.abs(level.percent).toFixed(2)}%`
  const price = level.price != null ? `${level.price.toLocaleString("en-US", { minimumFractionDigits: 2, maximumFractionDigits: 2 })} (${percent})` : percent
  return level.touch_probability != null ? `${price} · ${Math.round(level.touch_probability * 100)}% touch` : price
}

export const betaSources: Record<BetaSource, string> = {
  reference: "The reference itself",
  given: "Entered",
  index: "Same index",
  daily: "Daily closes",
  intraday: "5-minute closes this session",
}

/** "QQQ:1.2, AAPL:0.9" as overrides; null when any entry does not parse. */
export function parseBetas(text: string): Record<string, number> | null {
  const betas: Record<string, number> = {}
  for (const part of text.split(",").map((p) => p.trim()).filter(Boolean)) {
    const match = /^([A-Za-z0-9.]{1,16})\s*:\s*(-?\d+(?:\.\d+)?)$/.exec(part)
    if (!match) return null
    const beta = Number(match[2])
    if (!Number.isFinite(beta) || Math.abs(beta) > 10) return null
    betas[match[1]!.toUpperCase()] = beta
  }
  return betas
}
