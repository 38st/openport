import type { FeeModel, Money } from "../api/trading-types"
import { formatMoney } from "../lib/trading"

export function FeeModelPicker({ value, onChange, disabled, flat }: {
  value: FeeModel; onChange: (model: FeeModel) => void; disabled: boolean; flat?: Money
}) {
  return <div className="space-y-1">
    <label className="block space-y-1 text-sm">
      <span>Fees</span>
      <select aria-label="Fees" className="trade-input w-full" value={value} disabled={disabled}
        onChange={(event) => onChange(event.target.value as FeeModel)}>
        <option value="flat">Flat per contract</option>
        <option value="itemized">Itemized broker fees</option>
      </select>
    </label>
    <p className="text-xs text-muted">{value === "itemized"
      ? "$1.00 per contract to open, $0 to close; commission capped at $10 per leg per order. Every contract pays $0.10 clearing and $0.02 regulatory, plus $0.60 on SPX and SPXW. Exercise, assignment and delivery cost $5 per contract; cash settlement is free."
      : `${flat ? formatMoney(flat) : "The server's fee"} per contract, opening or closing. Exercise, assignment and settlement are free.`}</p>
  </div>
}
