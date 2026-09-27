import type { StrategyTemplate, TemplateTarget } from "./strategy"

export interface StrategyPreset { name: string; template: StrategyTemplate }
const key = (underlying: string) => `openport.strategy-presets.${underlying.toUpperCase()}`
const number = (value: unknown): value is number => typeof value === "number" && Number.isFinite(value)
function target(value: unknown, modes: TemplateTarget["mode"][]): boolean {
  if (!value || typeof value !== "object" || !("mode" in value) || !modes.includes(value.mode as TemplateTarget["mode"])) return false
  return value.mode === "atm" || ("value" in value && number(value.value) && (value.mode !== "delta" || (value.value > 0 && value.value < 100)))
}
function valid(value: unknown): value is StrategyTemplate {
  if (!value || typeof value !== "object" || !("kind" in value)) return false
  const t = value as Record<string, unknown>
  const type = t.type === "call" || t.type === "put"
  const width = number(t.width) && t.width > 0
  const side = t.side === "buy" || t.side === "sell"
  switch (t.kind) {
    case "vertical": return type && width && (t.direction === "credit" || t.direction === "debit") && target(t.target, ["delta", "points", "moves"])
    case "condor": return width && target(t.target, ["delta", "moves"])
    case "iron-butterfly": return width
    case "strangle": return side && number(t.delta) && t.delta > 0 && t.delta < 50
    case "straddle": return side
    case "butterfly": return type && width && target(t.target, ["atm", "strike"])
    case "calendar": case "diagonal": return type && target(t.target, ["atm", "delta"]) && typeof t.farExpiry === "string" && number(t.offset)
    default: return false
  }
}

/** Invalid or older entries are ignored; blocked storage leaves an in-memory choice. */
export function loadStrategyPresets(underlying: string): StrategyPreset[] {
  try {
    const saved: unknown = JSON.parse(localStorage.getItem(key(underlying)) ?? "[]")
    if (!Array.isArray(saved)) return []
    return saved.filter((p): p is StrategyPreset => p && typeof p.name === "string" && p.name.trim().length > 0 && valid(p.template))
  } catch { return [] }
}

export function saveStrategyPresets(underlying: string, presets: readonly StrategyPreset[]): boolean {
  try { localStorage.setItem(key(underlying), JSON.stringify(presets)); return true } catch { return false }
}
