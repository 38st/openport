import type { Trade } from "../api/trading-types"

/** Journal analytics are display summaries in dollars; accounting stays exact on the server. */
const dollars = (value: string | null | undefined) => {
  const parsed = value == null ? NaN : Number(value)
  return Number.isFinite(parsed) ? parsed : 0
}
export const tradeNet = (trade: Trade) => dollars(trade.net)

export interface JournalStats {
  trades: number
  wins: number
  losses: number
  /** Wins over decided (nonzero) trades. */
  winRate: number | null
  net: number
  grossWin: number
  grossLoss: number
  /** Gross wins over gross losses; Infinity with wins and no losses. */
  profitFactor: number | null
  averageWin: number | null
  averageLoss: number | null
  best: Trade | null
  worst: Trade | null
  contracts: number
  averageHoldSeconds: number | null
  fees: number
}

export function journalStats(trades: readonly Trade[]): JournalStats {
  const closed = trades.filter((t) => t.status === "closed")
  let wins = 0, losses = 0, grossWin = 0, grossLoss = 0, contracts = 0, fees = 0, hold = 0, held = 0
  let best: Trade | null = null, worst: Trade | null = null
  for (const trade of closed) {
    const net = tradeNet(trade)
    if (net > 0) { wins++; grossWin += net } else if (net < 0) { losses++; grossLoss += net }
    if (!best || net > tradeNet(best)) best = trade
    if (!worst || net < tradeNet(worst)) worst = trade
    contracts += trade.opened_contracts
    fees += dollars(trade.fees)
    if (trade.duration_seconds != null) { hold += trade.duration_seconds; held++ }
  }
  return {
    trades: closed.length, wins, losses,
    winRate: wins + losses > 0 ? wins / (wins + losses) : null,
    net: grossWin + grossLoss, grossWin, grossLoss,
    profitFactor: grossLoss < 0 ? grossWin / -grossLoss : wins > 0 ? Infinity : null,
    averageWin: wins ? grossWin / wins : null,
    averageLoss: losses ? grossLoss / losses : null,
    best, worst, contracts, fees,
    averageHoldSeconds: held ? hold / held : null,
  }
}

const newYorkParts = new Intl.DateTimeFormat("en-CA", { timeZone: "America/New_York", year: "numeric", month: "2-digit", day: "2-digit", weekday: "short" })
/** ISO timestamp -> New York trading date "YYYY-MM-DD" and weekday (0 = Sunday). */
export function newYorkDate(iso: string): { date: string; weekday: number } | null {
  const time = Date.parse(iso)
  if (!Number.isFinite(time)) return null
  const parts = Object.fromEntries(newYorkParts.formatToParts(time).map((p) => [p.type, p.value]))
  return { date: `${parts.year}-${parts.month}-${parts.day}`, weekday: ["Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"].indexOf(parts.weekday ?? "") }
}

export interface DayResult { date: string; net: number; trades: number; wins: number }
/** Closed trades by the New York date they closed. */
export function dailyResults(trades: readonly Trade[]): Map<string, DayResult> {
  const days = new Map<string, DayResult>()
  for (const trade of trades) {
    if (trade.status !== "closed" || !trade.closed) continue
    const day = newYorkDate(trade.closed)
    if (!day) continue
    const cell = days.get(day.date) ?? { date: day.date, net: 0, trades: 0, wins: 0 }
    const net = tradeNet(trade)
    cell.net += net
    cell.trades++
    if (net > 0) cell.wins++
    days.set(day.date, cell)
  }
  return days
}

/** Sunday-first weeks of a month as ISO dates, with null padding. month is 1-12. */
export function monthWeeks(year: number, month: number): (string | null)[][] {
  const first = new Date(Date.UTC(year, month - 1, 1))
  const length = new Date(Date.UTC(year, month, 0)).getUTCDate()
  const cells: (string | null)[] = Array(first.getUTCDay()).fill(null)
  for (let day = 1; day <= length; day++) cells.push(`${year}-${String(month).padStart(2, "0")}-${String(day).padStart(2, "0")}`)
  while (cells.length % 7) cells.push(null)
  const weeks: (string | null)[][] = []
  for (let i = 0; i < cells.length; i += 7) weeks.push(cells.slice(i, i + 7))
  return weeks
}

export type Dimension = "duration" | "weekday" | "month"
export type Side = "all" | "call" | "put"
export const durationBuckets = [
  { label: "< 1m", max: 60 }, { label: "1–5m", max: 300 }, { label: "5–15m", max: 900 },
  { label: "15m–1h", max: 3600 }, { label: "1–4h", max: 14_400 }, { label: "4h–1d", max: 86_400 },
  { label: "1–3d", max: 259_200 }, { label: "3d+", max: Infinity },
] as const
const weekdays = ["Mon", "Tue", "Wed", "Thu", "Fri"]
const months = ["Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"]

export interface Bucket { label: string; net: number; trades: number; wins: number; winRate: number | null }
/** Closed trades grouped by holding time, closing weekday or closing month. */
export function tradeBuckets(trades: readonly Trade[], dimension: Dimension, side: Side = "all"): Bucket[] {
  const labels = dimension === "duration" ? durationBuckets.map((b) => b.label) : dimension === "weekday" ? weekdays : months
  const out = labels.map((label) => ({ label, net: 0, trades: 0, wins: 0, winRate: null as number | null }))
  for (const trade of trades) {
    if (trade.status !== "closed" || !trade.closed || (side !== "all" && trade.type !== side)) continue
    let index = -1
    if (dimension === "duration") {
      const seconds = trade.duration_seconds ?? 0
      index = durationBuckets.findIndex((b) => seconds < b.max)
    } else {
      const day = newYorkDate(trade.closed)
      if (!day) continue
      index = dimension === "weekday" ? day.weekday - 1 : Number(day.date.slice(5, 7)) - 1
    }
    const bucket = out[index]
    if (!bucket) continue
    const net = tradeNet(trade)
    bucket.net += net
    bucket.trades++
    if (net > 0) bucket.wins++
  }
  for (const bucket of out) bucket.winRate = bucket.trades ? bucket.wins / bucket.trades : null
  return out
}

/** 290 -> "4m 50s"; 7500 -> "2h 5m"; 273600 -> "3d 4h". */
export function formatDuration(seconds: number | null | undefined): string {
  if (seconds == null || !Number.isFinite(seconds) || seconds < 0) return "—"
  const s = Math.round(seconds)
  if (s < 60) return `${s}s`
  if (s < 3600) return `${Math.floor(s / 60)}m${s % 60 ? ` ${s % 60}s` : ""}`
  if (s < 86_400) return `${Math.floor(s / 3600)}h${Math.floor(s % 3600 / 60) ? ` ${Math.floor(s % 3600 / 60)}m` : ""}`
  return `${Math.floor(s / 86_400)}d${Math.floor(s % 86_400 / 3600) ? ` ${Math.floor(s % 86_400 / 3600)}h` : ""}`
}

/** Canonical padded OSI -> its terms; null when malformed. */
export function parseOsi(symbol: string): { root: string; expiry: string; type: "call" | "put"; strike: number } | null {
  const match = /^(.{6})(\d{2})(\d{2})(\d{2})([CP])(\d{8})$/.exec(symbol)
  if (!match) return null
  const [, root, yy, mm, dd, kind, strike] = match
  return { root: root!.trim(), expiry: `20${yy}-${mm}-${dd}`, type: kind === "C" ? "call" : "put", strike: Number(strike) / 1000 }
}
/** Order and fill rows: "SPX Oct 22 5000C", falling back to the raw symbol. */
export function osiLabel(symbol: string, underlying: string): string {
  const terms = parseOsi(symbol)
  return terms ? contractLabel({ underlying: underlying || terms.root, expiry: terms.expiry, strike: terms.strike, type: terms.type }) : symbol
}

/** "SPXW  261022C05000000"-style trade -> "SPX Oct 22 5000C". */
export function contractLabel(t: { underlying: string; expiry: string; strike: number; type: "call" | "put" }): string {
  const date = new Date(`${t.expiry}T12:00:00Z`)
  const day = date.toLocaleDateString("en-US", { month: "short", day: "numeric", timeZone: "UTC" })
  return `${t.underlying} ${day} ${t.strike}${t.type === "call" ? "C" : "P"}`
}
