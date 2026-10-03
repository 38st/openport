import type { FillModel } from "../api/trading-types"

export function FillModelPicker({ value, onChange, disabled }: { value: FillModel; onChange: (value: FillModel) => void; disabled?: boolean }) {
  return <>
    <label className="block space-y-1 text-sm"><span>Simulated fills</span>
      <select className="trade-input w-full" value={value} disabled={disabled} onChange={(e) => onChange(e.target.value as FillModel)}>
        <option value="as_displayed">As displayed</option>
        <option value="conservative">Conservative</option>
        <option value="midpoint">Inside at midpoint (50%)</option>
      </select>
    </label>
    <p className="text-xs text-muted">{value === "conservative"
      ? "For delayed feeds: wait 1,000 ms on market time, add 1 tick of slippage, and 1 extra tick for each additional displayed-size block. The next available quote may arrive much later."
      : value === "midpoint" ? "Limits at or beyond halfway across the spread fill at their limit inside it, up to the displayed size left. Marketable orders use the bid or ask."
        : "Fill immediately at the displayed bid or ask, up to the available displayed size, with no slippage."}
      {" "}Applies to this account’s new attempt. These models do not simulate queue position or hidden liquidity.</p>
  </>
}
