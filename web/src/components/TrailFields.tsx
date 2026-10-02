import { useState, type ReactNode } from "react"
import type { Trail, TriggerReference } from "../api/trading-types"
import { validMoney } from "../lib/trading"

/** A decimal string with at least two fractional digits, as the API's money fields are. */
const decimalText = (value: string) => {
  const [whole, fraction = ""] = value.split(".")
  return `${whole}.${fraction.padEnd(2, "0")}`
}

/** What a stop adds to its trigger: what it reads and how it trails. */
export interface StopTrail {
  terms: { reference?: TriggerReference; trail?: Trail }
  valid: boolean
  fields: ReactNode
}

/**
 * A stop's reference (bid/ask, mid or mark, for option and combo stops) and an
 * optional trail by dollars, percent or ticks (not ticks of the underlying).
 */
export function useStopTrail(source: "option" | "underlying" | "combo", label = "Stop"): StopTrail {
  const [chosenReference, setReference] = useState<TriggerReference>("bid_ask")
  const [on, setOn] = useState(false)
  const [chosenUnit, setUnit] = useState<Trail["unit"]>("amount")
  const [value, setValue] = useState("")
  const spot = source === "underlying"
  const reference = spot ? "bid_ask" : chosenReference
  const unit = spot && chosenUnit === "ticks" ? "amount" : chosenUnit
  const number = Number(value)
  const valueValid = unit === "ticks" ? /^\d+$/.test(value) && number >= 1 && number <= 1000
    : validMoney(value) && /^\d+(\.\d{1,6})?$/.test(value) && number > 0 && (unit === "amount" || number < 100)
  const trail: Trail | undefined = !on || !valueValid ? undefined
    : unit === "ticks" ? { unit, value: number } : { unit, value: decimalText(value) }
  const terms = { ...(reference !== "bid_ask" ? { reference } : {}), ...(trail ? { trail } : {}) }
  const sideLabel = source === "combo" ? "Closing far sides" : "Bid / ask"
  const fields = <>
    {!spot && <label className="trade-label">{label} reads<select className="trade-input" value={reference} onChange={(e) => setReference(e.target.value as TriggerReference)}>
      <option value="bid_ask">{sideLabel}</option><option value="mid">Mid</option><option value="mark">Mark</option>
    </select></label>}
    <label className="flex items-center gap-2 text-xs"><input type="checkbox" checked={on} onChange={(e) => setOn(e.target.checked)} className="accent-[var(--accent)]" />Trailing</label>
    {on && <div className="grid grid-cols-2 gap-2">
      <label className="trade-label">Trail by<select className="trade-input" value={unit} onChange={(e) => setUnit(e.target.value as Trail["unit"])}>
        <option value="amount">Dollars</option><option value="percent">Percent</option>{!spot && <option value="ticks">Ticks</option>}
      </select></label>
      <label className="trade-label">Trail {unit === "amount" ? "($)" : unit === "percent" ? "(%)" : "(ticks)"}
        <input className="trade-input" inputMode={unit === "ticks" ? "numeric" : "decimal"} value={value} onChange={(e) => setValue(e.target.value)} /></label>
      <span className="col-span-2 text-[11px] text-muted">{valueValid
        ? "While armed in the regular session, the level follows the market at this distance as it moves your way, and never moves back. A stop-limit’s limit moves with it."
        : unit === "ticks" ? "Enter 1 to 1,000 ticks." : unit === "percent" ? "Enter a percentage above 0 and below 100." : "Enter a positive amount."}</span>
    </div>}
  </>
  return { terms, valid: !on || valueValid, fields }
}
