import { afterEach, describe, expect, it, vi } from "vitest"
import { api } from "../api/client"
import { templateChain } from "../test/template-fixtures"
import { buildTemplate, templateTag, type StrategyTemplate } from "./strategy"
import parity from "../../../tests/data/template-parity.json"

afterEach(() => vi.restoreAllMocks())
describe("server strategy templates", () => {
  it("delegates selection to the shared server picker", async () => {
    const template: StrategyTemplate = { kind: "straddle", side: "sell" }
    const reply = { reason: "Selected contract has no valid two-sided quote." }
    vi.spyOn(api, "buildTemplate").mockResolvedValue(reply)
    expect(await buildTemplate(template, templateChain)).toEqual(reply)
    expect(api.buildTemplate).toHaveBeenCalledWith(template, templateChain)
  })
  it("keeps server failures visible without inventing legs", async () => {
    vi.spyOn(api, "buildTemplate").mockRejectedValue(new Error("Chain unavailable"))
    expect(await buildTemplate({ kind: "straddle", side: "buy" }, templateChain)).toEqual({ reason: "Chain unavailable" })
  })
  it.each(parity.cases.filter((item) => "legs" in item.result))("keeps the template tag compatible for $template.kind", (item) => {
    expect(templateTag(item.template as StrategyTemplate)).toBe(item.result.tag)
  })
})
