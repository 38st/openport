import { describe, expect, it } from "vitest"
import { liquidity, liquidityDetail, spreadShare, volumeOiRatio } from "./liquidity"

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
