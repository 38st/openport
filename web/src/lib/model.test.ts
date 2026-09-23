import { describe, expect, it } from "vitest"
import { americanApproximation } from "./model"

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
