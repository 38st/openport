import type { FeeSchedule, FillFees } from "../api/trading-types"
import { formatMoney } from "./trading"

export function feeItems(f: FillFees): string {
  return `Commission ${formatMoney(f.commission)} · clearing ${formatMoney(f.clearing)} · regulatory ${formatMoney(f.regulatory)} · index ${formatMoney(f.index)}`
}
export function feeScheduleText(f: FeeSchedule): string {
  const cap = Number(f.leg_cap) > 0 ? `, capped at ${formatMoney(f.leg_cap)} per leg per order across partial fills` : ", uncapped"
  const index = Object.entries(f.index).map(([root, fee]) => `${formatMoney(fee)} on ${root}`).join(", ")
  return `Commission is ${formatMoney(f.open)} per contract to open and ${formatMoney(f.close)} to close${cap}. ` +
    `Every contract also pays ${formatMoney(f.clearing)} clearing and ${formatMoney(f.regulatory)} regulatory${index ? `; index fees: ${index}` : ""}. ` +
    `Exercise, assignment and delivery at expiry cost ${formatMoney(f.exercise)} per contract; cash settlement is free.`
}
