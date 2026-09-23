import { describe, expect, it } from "vitest"
import { groupKeyIndex } from "./keyboard"

describe("group keyboard navigation", () => {
  it("moves and wraps in both directions", () => {
    expect(groupKeyIndex("ArrowRight", 0, 3)).toBe(1)
    expect(groupKeyIndex("ArrowDown", 2, 3)).toBe(0)
    expect(groupKeyIndex("ArrowLeft", 0, 3)).toBe(2)
    expect(groupKeyIndex("ArrowUp", 2, 3)).toBe(1)
  })

  it("supports Home/End and leaves unrelated shortcuts alone", () => {
    expect(groupKeyIndex("Home", 2, 3)).toBe(0)
    expect(groupKeyIndex("End", 0, 3)).toBe(2)
    for (const key of ["1", "4", "Tab", "Enter", " "]) expect(groupKeyIndex(key, 0, 3)).toBeNull()
    expect(groupKeyIndex("ArrowRight", 0, 0)).toBeNull()
    expect(groupKeyIndex("ArrowLeft", 0, 1)).toBe(0)
  })
})
