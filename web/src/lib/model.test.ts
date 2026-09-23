import { describe, expect, it } from "vitest"
import { americanApproximation, rateSourceHint } from "./model"

describe("American approximation note", () => {
  it("prefers the selected expiry style over the summary and root heuristic", () => {
    expect(americanApproximation("SPX", false, "american")).toBe(true)
    expect(americanApproximation("SPY", true, "european")).toBe(false)
  })

  it("uses explicit summary flags when the expiry style is unavailable", () => {
    expect(americanApproximation("SPX", true, undefined)).toBe(true)
    expect(americanApproximation("SPY", false, null)).toBe(false)
  })

  it("falls back to the index-root heuristic only without metadata", () => {
    expect(americanApproximation("SPX", undefined, undefined)).toBe(false)
    expect(americanApproximation("SPXW", null, null)).toBe(false)
    expect(americanApproximation("SPY", undefined, undefined)).toBe(true)
  })
})

describe("rate provenance", () => {
  it("explains all rate sources and retains the old-payload fallback", () => {
    const rate = (rate_source?: "parity" | "term" | "curve" | "assumed", rate_fitted = false) =>
      rateSourceHint({ rate_source, rate_fitted, rate_curve_symbol: "SPX" } as import("../api/types").Expiry)
    expect(rate("parity")).toBe("Fitted from put-call parity")
    expect(rate("term")).toBe("Borrowed from this underlying's longer expiries")
    expect(rate("curve")).toContain("From the SPX parity curve")
    expect(rate("assumed")).toBe("Assumed flat rate: add SPX to get a market-implied curve")
    expect(rate(undefined, true)).toBe(rate("parity"))
    expect(rate()).toBe(rate("term"))
    expect(americanApproximation("SPY", true, "american", true)).toBe(false)
    expect(americanApproximation("SPY", true, "american", false)).toBe(true)
  })
})
