import parity from "../../../tests/data/template-parity.json"
import type { Chain } from "../api/types"
import type { StrategyTemplate, TemplateResult, TemplateSetup } from "../lib/strategy"
import { templateChain, farTemplateChain } from "./template-fixtures"
const canonical = (value: unknown): string => {
  if (value == null || typeof value !== "object") return JSON.stringify(value)
  if (Array.isArray(value)) return `[${value.map(canonical).join(",")}]`
  return JSON.stringify(Object.keys(value).sort().map((key) => [key, canonical((value as Record<string, unknown>)[key])]))
}
/** Golden responses captured from the old picker; C++ checks these same cases. */
export async function templateServer(template: StrategyTemplate, near: Chain): Promise<TemplateResult> {
  const fixture = parity.cases.find((item) => canonical(item.template) === canonical(template) && item.near === 0 && item.far === 1)
  if (!fixture) return { reason: "No fixture for this template" }
  if ("reason" in fixture.result) return { reason: fixture.result.reason! }
  const result = fixture.result as Omit<TemplateSetup, "legs"> & { legs: Omit<TemplateSetup["legs"][number], "quote">[] }
  return { ...result, legs: result.legs.map((leg) => {
    const chain = leg.expiry === near.expiry.id ? templateChain : farTemplateChain
    const quote = chain.strikes.find((row) => row.strike === leg.strike)?.[leg.type] ?? null
    return { ...leg, quote }
  }) }
}
