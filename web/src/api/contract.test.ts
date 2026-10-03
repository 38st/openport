import specText from "../../../docs/openapi.yaml?raw"
import marketTypes from "./types.ts?raw"
import playbookTypes from "./playbook-types.ts?raw"
import tradingTypes from "./trading-types.ts?raw"
import { describe, expect, it } from "vitest"

// openapi.yaml uses JSON syntax, a YAML subset; no YAML dependency is needed.
interface Schema { $ref?: string; type?: string; const?: string; anyOf?: Schema[]; items?: Schema; properties?: Record<string, Schema>; required?: string[]; additionalProperties?: Schema | boolean }
const spec = JSON.parse(specText) as { components: { schemas: Record<string, Schema> } }
const source = [marketTypes, tradingTypes].join("\n")
  .replace(/\/\*[\s\S]*?\*\/|\/\/[^\n]*/g, "")
const normalize = (value: string) => value.split("|").map((part) => part.trim()).sort().join(" | ")
function wireType(schema: Schema): string {
  if (schema.$ref) return schema.$ref.split("/").at(-1)!
  if (schema.anyOf) return normalize(schema.anyOf.map(wireType).join(" | "))
  if (schema.items) return `${wireType(schema.items)}[]`
  if (schema.type === "object" && typeof schema.additionalProperties === "object") return `Record<string, ${wireType(schema.additionalProperties)}>`
  if (schema.const !== undefined) return JSON.stringify(schema.const)
  return schema.type!
}

describe("checked API core types", () => {
  it("keeps position disposal fields and closure kinds in sync", () => {
    const position = /export interface Position \{([\s\S]*?)\n\}/.exec(source)![1]!
    for (const field of ["no_bid", "do_not_exercise"]) {
      expect(position).toMatch(new RegExp(`${field}\\?: boolean`))
      expect(wireType(spec.components.schemas.Position!.properties![field]!)).toBe("boolean")
    }
    expect(normalize(/settle_by\?: ([^\n]+)/.exec(position)![1]!)).toBe(wireType(spec.components.schemas.Position!.properties!.settle_by!))
    const trade = /export interface Trade \{([\s\S]*?)\n\}/.exec(source)![1]!
    expect(normalize(/closure: ([^\n]+)/.exec(trade)![1]!)).toBe(wireType(spec.components.schemas.Trade!.properties!.closure!))
    for (const field of ["settlement_value", "settlement_source"]) {
      expect(normalize(new RegExp(`${field}\\?: ([^\\n]+)`).exec(trade)![1]!)).toBe(wireType(spec.components.schemas.Trade!.properties![field]!))
    }
    const exits = /export type TradeExit = ([^\n]+)/.exec(source)![1]!
    expect(normalize(`${exits} | null`)).toBe(wireType(spec.components.schemas.Trade!.properties!.closed_by!))
  })
  it("account margin fields match the rules and both request contracts", () => {
    const rules = spec.components.schemas.AccountRules!.properties!
    expect(wireType(rules.account_type!)).toBe(normalize('"margin" | "cash" | "ira"'))
    expect(wireType(rules.margin!)).toBe(normalize('"strategy" | "portfolio"'))
    for (const key of ["house_margin_percent", "pm_vol_shock"]) expect(wireType(rules[key]!)).toBe("integer")
    const accountType = /export type AccountType = ([^\n]+)/.exec(source)![1]!
    expect(normalize(accountType)).toBe(wireType(rules.account_type!))
    const model = /export interface MarginModel \{([^}]+)\}/.exec(source)![1]!
    for (const field of ["margin", "account_type", "house_margin_percent", "pm_vol_shock"]) {
      expect(model).toMatch(new RegExp(`${field}\\?:`))
      expect(source).toMatch(/export type ResetRequest = [^\n]+& MarginModel/)
      expect(source).toMatch(/export type CreateAccountRequest = [^\n]+& MarginModel/)
    }
  })
  it.each(["Candle", "OptionQuote", "ChainRow", "Fill", "OrdersResponse", "EquitySample", "NotificationChannel", "NotificationStatus", "Alert", "AlertsResponse", "AlertResponse", "AlertDeleted",
    "SettlementSource", "SettlementRecord", "SettlementsResponse", "StockPreview", "MarginLeg", "MarginPart", "MarginScan", "MarginUnderlying", "FillFees", "FeeSchedule", "PriceRange", "PriceOdds", "ProbabilityHorizon", "Probability",
    "ProfileLevel", "ProfileHorizon", "Walk", "WalkStep"])("%s matches OpenAPI fields, types and nullability", (name) => {
    const body = new RegExp(`export interface ${name} \\{([^}]+)\\}`).exec(source)?.[1]
    expect(body).toBeDefined()
    const fields = [...body!.matchAll(/(\w+)(\?)?\s*:\s*([^;\n}]+)/g)]
    const schema = spec.components.schemas[name]!
    expect(fields.map((field) => field[1]).sort()).toEqual(Object.keys(schema.properties!).sort())
    for (const [, field, optional, type] of fields) {
      // TypeScript has no integer type: an OpenAPI integer is a number on the wire.
      expect(normalize(type!), `${name}.${field}`).toBe(wireType(schema.properties![field!]!).replace(/\binteger\b/g, "number"))
      // Browser types allow fields absent on older servers; all its required
      // fields must still be required by the current HTTP contract.
      if (!optional) expect(schema.required, `${name}.${field}`).toContain(field)
    }
  })
})

it("keeps extended TIFs and sparse order metadata in the wire contract", () => {
  const declared = /export type TimeInForce = ([^\n]+)/.exec(source)![1]!
  expect(normalize(declared)).toBe(wireType(spec.components.schemas.Order!.properties!.time_in_force!))
  for (const key of ["good_till", "limit_ticks"]) {
    expect(source).toContain(`${key}?:`)
    expect(spec.components.schemas.Order!.properties![key]).toBeDefined()
  }
})

it("keeps alert request fields and optional terms in sync", () => {
  const body = /export interface AlertRequest \{([^}]+)\}/.exec(source)![1]!
  const fields = [...body.matchAll(/(\w+)(\?)?\s*:\s*([^;\n}]+)/g)]
  const schema = spec.components.schemas.AlertRequest!
  const scope = /export type AlertScope = ([^\n]+)/.exec(source)![1]!
  expect(fields.map((field) => field[1]).sort()).toEqual(Object.keys(schema.properties!).sort())
  for (const [, field, optional, type] of fields) {
    expect(normalize(type === "AlertScope" ? scope : type!)).toBe(wireType(schema.properties![field!]!))
    expect(schema.required?.includes(field!)).toBe(!optional)
  }
})


it("keeps the preview sizing basis and nullability in sync", () => {
  const declared = /max_units_basis\?: ([^\n]+)/.exec(source)![1]!
  expect(normalize(declared)).toBe(wireType(spec.components.schemas.OrderPreview!.properties!.max_units_basis!))
  expect(spec.components.schemas.OrderPreview!.required).toContain("max_units_basis")
})

it("publishes a stage HTTP order separately from the legacy request", () => {
  const stage = spec.components.schemas.StagedOrder!.properties!
  expect(wireType(stage.order!)).toBe("NewOrder")
  expect(wireType(stage.request!)).toBe("object")
  expect(playbookTypes).toContain("order?: NewOrder")
  expect(playbookTypes).toContain("request?: Record<string, unknown>")
})
