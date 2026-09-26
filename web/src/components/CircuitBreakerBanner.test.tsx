import { renderToStaticMarkup } from "react-dom/server"
import { describe, expect, it, vi } from "vitest"
import { liveState, useLive } from "../api/live"
import type { CircuitBreaker, Tick } from "../api/types"
import { status } from "../test/trading-fixtures"
import { CircuitBreakerBanner } from "./CircuitBreakerBanner"

vi.mock("../api/live", async (original) => ({ ...await original<typeof import("../api/live")>(), useLive: vi.fn() }))

const breaker: CircuitBreaker = {
  symbol: "SPX", day: "2026-09-22", previous_close: { date: "2026-09-21", price: 5400 }, level: 1,
  market_time: "2026-09-22T14:05:00Z", active: true, error: null,
  halts: [{ level: 1, start: "2026-09-22T14:00:00Z", end: "2026-09-22T14:15:00Z", reference: 5400, price: 5000, active: true }],
}
function render(state: CircuitBreaker | null | undefined, tick: Tick | null = null) {
  vi.mocked(useLive).mockReturnValue(liveState({ ...status, circuit_breaker: state }, tick, "open"))
  return renderToStaticMarkup(<CircuitBreakerBanner />).replaceAll("&amp;", "&")
}

describe("the circuit breaker banner", () => {
  it("describes the recorded fall and resume time from REST, independently of wall time", () => {
    const html = render(breaker)
    expect(html).toContain('role="status"')
    expect(html).toContain("Trading is halted market-wide: the S&P 500 fell 7.4% from its previous close of 5,400.00 (a level 1 circuit breaker).")
    expect(html).toContain("Trading resumes at 10:15 ET.")
    // A revised reference cannot rewrite the print that caused the halt.
    expect(render({ ...breaker, previous_close: { date: "2026-09-21", price: 5500 } })).toContain("5,400.00")
  })

  it("uses Eastern standard time in winter and identifies the SPY proxy", () => {
    const html = render({ ...breaker, symbol: "SPY", halts: [{ ...breaker.halts[0]!, reference: 540, price: 500,
      start: "2026-12-01T15:00:00Z", end: "2026-12-01T15:15:00Z" }] })
    expect(html).toContain("SPY, standing in for the S&P 500, fell 7.4%")
    expect(html).toContain("previous close of 540.00")
    expect(html).toContain("Trading resumes at 10:15 ET.")
  })

  it("uses the highest active level and describes level 3 as the rest of the day", () => {
    const second = { ...breaker.halts[0]!, level: 2, price: 4600, end: "2026-09-22T14:20:00Z" }
    expect(render({ ...breaker, level: 2, halts: [...breaker.halts, second] })).toContain("a level 2 circuit breaker")
    const html = render({ ...breaker, level: 3, halts: [...breaker.halts, { ...second, level: 3, price: 4200 }] })
    expect(html).toContain("a level 3 circuit breaker")
    expect(html).toContain("for the rest of the day.")
    expect(html).not.toContain("resumes at")
  })

  it("disappears when ticks end a halt and stays hidden for missing or inactive state", () => {
    const tick: Tick = { type: "tick", feed: status.feed, underlyings: status.underlyings, engine: status.engine,
      circuit_breaker: { ...breaker, active: false, halts: [{ ...breaker.halts[0]!, active: false }] } }
    expect(render(breaker, tick)).toBe("")
    expect(render(null)).toBe("")
    expect(render(undefined)).toBe("")
    expect(render({ ...breaker, active: false })).toBe("")
    expect(render({ ...breaker, halts: [] })).toBe("")
    tick.circuit_breaker = breaker
    expect(render(undefined, tick)).toContain("Trading is halted market-wide")
  })
})
