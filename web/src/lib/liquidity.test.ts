import { describe, expect, it } from "vitest"
import type { PreviewLiquidity } from "../api/trading-types"
import { displayedSizeNotes, liquidity, liquidityDetail, quoteIssueText, spreadShare, volumeOiRatio } from "./liquidity"

const quote = { bid: 9, ask: 11, volume: 100, oi: 500 }
describe("liquidity cues", () => {
  it("uses spread divided by the quote mid, with an inclusive 20% good boundary", () => {
    expect(spreadShare(quote)).toBe(.2)
    expect(liquidity(quote).cue).toBe("good")
    expect(liquidity({ ...quote, ask: 11.01 }).cue).toBe("thin")
  })
  it("requires both known activity measures below their thresholds", () => {
    expect(liquidity({ ...quote, volume: 99, oi: 499 }).cue).toBe("thin")
    expect(liquidity({ ...quote, volume: 0 }).cue).toBe("good")
    expect(liquidity({ ...quote, oi: 0 }).cue).toBe("good")
    expect(liquidity({ ...quote, volume: 0, oi: 0 }).cue).toBe("thin")
  })
  it.each([null, undefined, NaN, -1])("does not treat missing or invalid activity as zero: %s", (volume) => {
    const missing = { ...quote, volume, oi: 0 }
    expect(liquidity(missing).cue).toBe("good")
    expect(liquidityDetail(missing)).toContain("Volume unknown")
    expect(volumeOiRatio(missing)).toBeNull()
  })
  it.each([null, { ...quote, bid: 0 }, { ...quote, ask: null }, { ...quote, bid: 12 }, { ...quote, bid: NaN }, { ...quote, ask: Infinity }])("has no two-sided liquidity for %s", (missing) => {
    expect(liquidity(missing).cue).toBe("none")
  })
  it("reports volume / OI only with a known positive denominator", () => {
    expect(volumeOiRatio(quote)).toBe(.2)
    expect(volumeOiRatio({ ...quote, volume: 0 })).toBe(0)
    expect(volumeOiRatio({ ...quote, oi: 0 })).toBeNull()
    expect(volumeOiRatio({ ...quote, oi: null })).toBeNull()
    expect(liquidityDetail(quote)).toContain("Spread 20.0% · Volume 100 · OI 500")
  })
})

describe("executable quotes and displayed size", () => {
  it("say why a quote cannot fill paper orders", () => {
    expect(quoteIssueText({ executable: true, quote_issue: null })).toBeNull()
    expect(quoteIssueText({})).toBeNull()  // an older server
    expect(quoteIssueText({ executable: false, quote_issue: "no_bid" })).toBe("One-sided: no bid")
    expect(quoteIssueText({ executable: false, quote_issue: "zero_size" })).toBe("A side shows no displayed size")
  })
  it("show the displayed size this account has used, and what the rest of an order does", () => {
    const leg: PreviewLiquidity = { symbol: "A", side: "buy", contracts: 10, executable: true, reason: null, displayed: 10, size_left: 4 }
    const label = () => "4900 put"
    expect(displayedSizeNotes([leg], label, { ioc: true, impact: false })).toEqual([
      "4900 put: 4 of the 10 displayed at the ask left for your paper orders until a new quote.",
      "4900 put: only 4 of 10 contracts can fill on this quote; the rest cancels (IOC).",
    ])
    expect(displayedSizeNotes([{ ...leg, side: "sell", contracts: 3 }], label, { ioc: false, impact: false }))
      .toEqual(["4900 put: 4 of the 10 displayed at the bid left for your paper orders until a new quote."])
    expect(displayedSizeNotes([{ ...leg, size_left: 10 }], label, { ioc: false, impact: false })).toEqual([])
    expect(displayedSizeNotes([{ ...leg, size_left: 10, contracts: 12 }], label, { ioc: false, impact: true }))
      .toEqual(["4900 put: 12 contracts against 10 left; the rest fills at simulated depth prices, within any limit."])
    expect(displayedSizeNotes([{ ...leg, executable: false, reason: { code: "INVALID_QUOTE", message: "No bid: the quote is one-sided" } }], label, { ioc: true, impact: false }))
      .toEqual(["4900 put: No bid: the quote is one-sided; new orders on it are refused until it can fill."])
  })
})
