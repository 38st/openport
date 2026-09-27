import { afterEach, describe, expect, it, vi } from "vitest"
import { liveState, marketTime } from "../api/live"
import type { Expiry, ReplayState, Tick } from "../api/types"
import { expiry, status } from "../test/trading-fixtures"
import { autoCloseCountdown, defaultExpiry } from "./expiry"

const today: Expiry = { ...expiry, id: "2026-09-23PM-SPXW", expiry: "2026-09-23", days: .01,
  expiry_time: "2026-09-23T20:00:00Z", last_trade: "2026-09-23T20:00:00Z", auto_close: "2026-09-23T19:55:00Z" }
const tomorrow: Expiry = { ...today, id: "2026-09-24PM-SPXW", expiry: "2026-09-24", days: 1,
  expiry_time: "2026-09-24T20:00:00Z", last_trade: "2026-09-24T20:00:00Z", auto_close: "2026-09-24T19:55:00Z" }
const at = (clock: string) => Date.parse(`2026-09-23T${clock}Z`)
afterEach(() => vi.restoreAllMocks())

describe("chain expiry clock", () => {
  it.each(["19:00:00", "19:54:00", "19:54:59"])("keeps 0DTE at %s UTC before auto-close", (clock) => {
    expect(defaultExpiry([today, tomorrow], at(clock))).toBe(today.id)
  })
  it.each(["19:55:00", "19:56:00"])("selects the next expiry at %s UTC after auto-close", (clock) => {
    expect(defaultExpiry([today, tomorrow], at(clock))).toBe(tomorrow.id)
  })
  it("keeps ETF options through 16:09 and skips them at 16:10 ET", () => {
    const etf = { ...today, last_trade: "2026-09-23T20:15:00Z", auto_close: "2026-09-23T20:10:00Z" }
    expect(defaultExpiry([etf, tomorrow], at("19:56:00"))).toBe(etf.id)
    expect(defaultExpiry([etf, tomorrow], at("20:09:59"))).toBe(etf.id)
    expect(defaultExpiry([etf, tomorrow], at("20:10:00"))).toBe(tomorrow.id)
  })
  it("skips AM series whose trading ended the previous day", () => {
    const am = { ...today, id: "AM", auto_close: "2026-09-22T20:10:00Z" }
    expect(defaultExpiry([am, today], at("19:00:00"))).toBe(today.id)
  })
  it("shows minutes and seconds only on the auto-close day in New York", () => {
    expect(autoCloseCountdown(today, at("19:42:26"))).toBe("auto-close in 12:34")
    expect(autoCloseCountdown(today, at("19:55:00"))).toBe("auto-close reached")
    expect(autoCloseCountdown(today, at("13:30:00"))).toBe("auto-close in 6:25:00")
    expect(autoCloseCountdown(tomorrow, at("19:54:00"))).toBeNull()
    expect(autoCloseCountdown(today, Date.parse("2026-09-23T01:00:00Z"))).toBeNull()
    expect(autoCloseCountdown(today, Number.NaN)).toBeNull()
  })
  it("uses connected ticks, and REST when disconnected, without wall-clock advancement", () => {
    vi.spyOn(Date, "now").mockReturnValue(at("20:30:00"))
    const tick: Tick = { type: "tick", feed: status.feed, engine: status.engine,
      underlyings: [{ ...status.underlyings[0]!, as_of: "2026-09-23T19:54:00Z" }] }
    const rest = { ...status, underlyings: [{ ...status.underlyings[0]!, as_of: "2026-09-23T19:00:00Z" }] }
    const now = marketTime(liveState(rest, tick, "open"), "SPX")
    expect(defaultExpiry([today, tomorrow], now)).toBe(today.id)
    expect(autoCloseCountdown(today, now)).toBe("auto-close in 1:00")
    expect(marketTime(liveState(rest, tick, "closed"), "SPX")).toBe(at("19:00:00"))
    expect(Date.now).not.toHaveBeenCalled()
  })
  it("uses the replay's effective market time even when its receipt and wall clocks are later", () => {
    vi.spyOn(Date, "now").mockReturnValue(Date.parse("2030-01-01T00:00:00Z"))
    const replay: ReplayState = { file: "fixture.oprec", provider: "cboe", symbols: ["SPX"], started: null,
      delay_seconds: 900, speed: 60, paused: true, finished: false, time: "2026-09-23T20:09:00Z" }
    const live = liveState({ ...status, underlyings: [{ ...status.underlyings[0]!, as_of: "2026-09-23T19:54:00Z" }] },
      null, "open", 0, "main", () => {}, "replay", replay)
    const now = marketTime(live, "SPX")
    expect(defaultExpiry([today, tomorrow], now)).toBe(today.id)
    expect(autoCloseCountdown(today, now)).toBe("auto-close in 1:00")
    expect(Date.now).not.toHaveBeenCalled()
  })
  it("supports old summaries and missing market timestamps without inventing a countdown", () => {
    expect(defaultExpiry([{ ...expiry, days: .001 }, tomorrow], Number.NaN)).toBe(expiry.id)
    expect(defaultExpiry([], Number.NaN)).toBeNull()
    expect(defaultExpiry([today], at("20:00:00"))).toBe(today.id)
    expect(autoCloseCountdown(expiry, at("19:00:00"))).toBeNull()
    expect(marketTime({ underlyings: [] }, "SPX", "2026-09-23T19:54:00Z")).toBe(at("19:54:00"))
    expect(marketTime({ underlyings: [] }, "SPX")).toBeNaN()
  })
})
