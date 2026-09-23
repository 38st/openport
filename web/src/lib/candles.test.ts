import { afterEach, describe, expect, it, vi } from "vitest"
import type { Candle } from "../api/types"
import type { Order } from "../api/trading-types"
import { order, portfolio } from "../test/trading-fixtures"
import { barTime, chartLevels, clampWindow, expectedMove, loadChartPrefs, priceDomain, saveChartPrefs, timeTicks } from "./candles"

/** 2026-09-22 09:30 ET. */
const open = Date.UTC(2026, 8, 22, 13, 30) / 1000
const minutes = (count: number, start = open): Candle[] =>
  Array.from({ length: count }, (_, i) => ({ t: start + i * 60, o: 7000 + i, h: 7001 + i, l: 6999 + i, c: 7000.5 + i }))
/** A daily bar at 09:30 ET of a winter date (UTC−5). */
const winterDay = (date: string): Candle => {
  const [y, m, d] = date.split("-").map(Number) as [number, number, number]
  return { t: Date.UTC(y, m - 1, d, 14, 30) / 1000, o: 1, h: 1, l: 1, c: 1 }
}

afterEach(() => vi.unstubAllGlobals())

describe("chart levels", () => {
  it("draws each held strike once, net long or short, and armed underlying triggers", () => {
    const stop: Order = { ...order, id: "stop", status: "armed", role: "stop_loss", side: "sell", remaining_quantity: 2,
      trigger: { source: "underlying", direction: "at_or_below", level: "6950.00" } }
    const optionStop: Order = { ...stop, id: "option-stop", trigger: { source: "option", direction: "at_or_below", level: "2.00" } }
    const elsewhere: Order = { ...stop, id: "spy", underlying: "SPY" }
    const short = { ...portfolio.positions[0]!, symbol: "SPXW  261016P07000000", type: "put" as const, quantity: -3 }
    expect(chartLevels("SPX", [...portfolio.positions, short], [stop, optionStop, elsewhere, order])).toEqual([
      { price: 7000, label: "7000 +2C Oct 16, −3P Oct 16", kind: "short" },
      { price: 6950, label: "Stop ≤ 6,950.00: SPX Oct 16 7000C ×2", kind: "trigger" },
      { price: 6800, label: "6800 −1P Sep 18", kind: "short" },
    ])
    const buy: Order = { ...stop, role: null, side: "buy", trigger: { source: "underlying", direction: "at_or_above", level: "7100" } }
    expect(chartLevels("SPX", [portfolio.positions[0]!], [buy])).toEqual([
      { price: 7100, label: "Buy ≥ 7,100.00: SPX Oct 16 7000C ×2", kind: "trigger" },
      { price: 7000, label: "7000 +2C Oct 16", kind: "long" },
    ])
    expect(chartLevels("SPY", portfolio.positions, [])).toEqual([])
  })

  it("puts one standard deviation at spot times ATM IV times root time", () => {
    expect(expectedMove(7000, 0.2, 36.5)).toBeCloseTo(7000 * 0.2 * Math.sqrt(0.1), 9)
    expect(expectedMove(null, 0.2, 10)).toBeNull()
    expect(expectedMove(7000, 0, 10)).toBeNull()
    expect(expectedMove(7000, 0.2, 0)).toBeNull()
  })
})

describe("chart window and scale", () => {
  it("keeps the view inside the bars and ends it offset bars before the latest", () => {
    expect(clampWindow(100, 40, 0)).toEqual({ span: 40, offset: 0, start: 60, end: 100 })
    expect(clampWindow(100, 40, 80)).toEqual({ span: 40, offset: 60, start: 0, end: 40 })
    expect(clampWindow(100, 2, -5)).toEqual({ span: 10, offset: 0, start: 90, end: 100 })
    expect(clampWindow(5, 40, 3)).toEqual({ span: 5, offset: 0, start: 0, end: 5 })
    expect(clampWindow(0, 40, 0)).toEqual({ span: 0, offset: 0, start: 0, end: 0 })
  })

  it("pads the price range so wicks never touch the frame", () => {
    expect(priceDomain([])).toBeNull()
    const [lo, hi] = priceDomain(minutes(3))!
    expect(lo).toBeCloseTo(6999 - 0.32, 9)
    expect(hi).toBeCloseTo(7003 + 0.32, 9)
    const [flatLo, flatHi] = priceDomain([{ t: 0, o: 100, h: 100, l: 100, c: 100 }])!
    expect(flatLo).toBeLessThan(100)
    expect(flatHi).toBeGreaterThan(100)
  })
})

describe("chart time axis", () => {
  it("labels round New York times and the date where a day starts", () => {
    expect(timeTicks(minutes(61), "1m", 13).map((t) => t.label)).toEqual(["09:30", "09:45", "10:00", "10:15", "10:30"])
    expect(timeTicks([...minutes(3), ...minutes(3, open + 86_400)], "1m", 1)).toEqual([
      { index: 0, label: "09:30", major: false },
      { index: 3, label: "Sep 23", major: true },
    ])
  })

  it("labels months on daily bars, and the year in January", () => {
    const days = ["2026-11-30", "2026-12-30", "2026-12-31", "2027-01-04"].map(winterDay)
    expect(timeTicks(days, "1d", 1)).toEqual([
      { index: 1, label: "Dec", major: true },
      { index: 3, label: "2027", major: true },
    ])
  })

  it("reads a bar's time in New York", () => {
    expect(barTime(open, "5m")).toBe("Sep 22 09:30")
    expect(barTime(open, "1d")).toBe("Tue, Sep 22, 2026")
  })
})

describe("chart preferences", () => {
  it("remember the interval and visibility, and fall back when storage fails", () => {
    const store = new Map<string, string>()
    vi.stubGlobal("localStorage", { getItem: (k: string) => store.get(k) ?? null, setItem: (k: string, v: string) => void store.set(k, v) })
    expect(loadChartPrefs()).toEqual({ interval: "5m", hidden: false })
    saveChartPrefs({ interval: "1d", hidden: true })
    expect(loadChartPrefs()).toEqual({ interval: "1d", hidden: true })
    store.set("openport.chart", JSON.stringify({ interval: "2m", hidden: "yes" }))
    expect(loadChartPrefs()).toEqual({ interval: "5m", hidden: false })
    vi.stubGlobal("localStorage", { getItem: () => { throw new Error("blocked") }, setItem: () => { throw new Error("blocked") } })
    expect(loadChartPrefs()).toEqual({ interval: "5m", hidden: false })
    expect(() => saveChartPrefs({ interval: "1m", hidden: false })).not.toThrow()
  })
})
