import specText from "../../../docs/openapi.yaml?raw"
import marketTypes from "./types.ts?raw"
import tradingTypes from "./trading-types.ts?raw"
import { describe, expect, it } from "vitest"

// openapi.yaml uses JSON syntax, a YAML subset; no YAML dependency is needed.
interface Schema { $ref?: string; type?: string; const?: string; anyOf?: Schema[]; items?: Schema; properties?: Record<string, Schema>; required?: string[] }
const spec = JSON.parse(specText) as { components: { schemas: Record<string, Schema> } }
const source = [marketTypes, tradingTypes].join("\n")
  .replace(/\/\*[\s\S]*?\*\/|\/\/[^\n]*/g, "")
const normalize = (value: string) => value.split("|").map((part) => part.trim()).sort().join(" | ")
function wireType(schema: Schema): string {
  if (schema.$ref) return schema.$ref.split("/").at(-1)!
  if (schema.anyOf) return normalize(schema.anyOf.map(wireType).join(" | "))
  if (schema.items) return `${wireType(schema.items)}[]`
  if (schema.const !== undefined) return JSON.stringify(schema.const)
  return schema.type!
}

describe("checked API core types", () => {
  it.each(["Candle", "OptionQuote", "ChainRow", "Fill", "OrdersResponse", "EquitySample", "NotificationChannel", "NotificationStatus",
    "MarginLeg", "MarginPart", "MarginScan", "MarginUnderlying"])("%s matches OpenAPI fields, types and nullability", (name) => {
    const body = new RegExp(`export interface ${name} \\{([^}]+)\\}`).exec(source)?.[1]
    expect(body).toBeDefined()
    const fields = [...body!.matchAll(/(\w+)(\?)?\s*:\s*([^;\n}]+)/g)]
    const schema = spec.components.schemas[name]!
    expect(fields.map((field) => field[1]).sort()).toEqual(Object.keys(schema.properties!).sort())
    for (const [, field, optional, type] of fields) {
      expect(normalize(type!), `${name}.${field}`).toBe(wireType(schema.properties![field!]!))
      // Browser types allow fields absent on older servers; all its required
      // fields must still be required by the current HTTP contract.
      if (!optional) expect(schema.required, `${name}.${field}`).toContain(field)
    }
  })
})
