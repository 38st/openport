import type { Attribution } from "../api/trading-types"
import { signedMoney } from "./trading"

export const attributionParts = [
  { key: "delta", label: "Delta", title: "The underlying's move times delta" },
  { key: "gamma", label: "Gamma", title: "Half gamma times the move squared" },
  { key: "vega", label: "Vega", title: "Implied volatility's change, in points, times vega" },
  { key: "theta", label: "Theta", title: "The days that passed times theta" },
  { key: "other", label: "Other", title: "What the Greeks leave unexplained: larger moves, the smile, marks without Greeks" },
  { key: "costs", label: "Costs", title: "The spread paid against the mark at each fill, and fees" },
] as const

const dollars = (value: number) => signedMoney(value.toFixed(2))
/** "Delta +$12.00 · Gamma +$0.50 · …", for a tooltip. */
export function describeAttribution(a: Attribution): string {
  return attributionParts.map((part) => `${part.label} ${dollars(a[part.key])}`).join(" · ")
}
