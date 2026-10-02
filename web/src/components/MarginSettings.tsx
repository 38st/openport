import type { AccountRules, AccountType, MarginModel } from "../api/trading-types"

/** The margin a new attempt or account starts with: the plan's own until changed. */
export interface MarginChoice {
  account_type: AccountType
  margin: "strategy" | "portfolio"
  house_margin_percent: number
  pm_vol_shock: number
}
export function planMargin(rules?: Pick<AccountRules, "account_type" | "margin" | "house_margin_percent" | "pm_vol_shock">): MarginChoice {
  return { account_type: rules?.account_type ?? "margin", margin: rules?.margin ?? "strategy",
    house_margin_percent: rules?.house_margin_percent ?? 0, pm_vol_shock: rules?.pm_vol_shock ?? 0 }
}
/** Only the settings that differ from the plan's, so a plain start sends nothing new. */
export function marginRequest(choice: MarginChoice, plan: MarginChoice): MarginModel {
  const cash = choice.account_type !== "margin"
  const margin = cash ? "strategy" : choice.margin
  const shock = margin === "portfolio" ? choice.pm_vol_shock : plan.pm_vol_shock
  return {
    ...(choice.account_type !== plan.account_type ? { account_type: choice.account_type } : {}),
    ...(margin !== plan.margin ? { margin } : {}),
    ...(choice.house_margin_percent !== plan.house_margin_percent ? { house_margin_percent: choice.house_margin_percent } : {}),
    ...(shock !== plan.pm_vol_shock ? { pm_vol_shock: shock } : {}),
  }
}
export function marginFacts(r: Pick<AccountRules, "account_type" | "margin" | "house_margin_percent" | "pm_vol_shock">): string[] {
  return [
    ...(r.account_type === "cash" ? ["Cash account"] : r.account_type === "ira" ? ["IRA (limited margin)"] : []),
    ...(r.margin === "portfolio" ? [`Portfolio margin${r.pm_vol_shock ? ` with ±${r.pm_vol_shock} vol points` : ""}`] : []),
    ...(r.house_margin_percent ? [`${r.house_margin_percent}% house margin`] : []),
  ]
}
const whole = (value: string, max: number) => Math.min(max, Math.max(0, Math.trunc(Number(value) || 0)))

/** Account type, margin mode, house margin and the portfolio vol shock, as a broker sets them. */
export function MarginSettings({ value, onChange, disabled }: { value: MarginChoice; onChange: (value: MarginChoice) => void; disabled?: boolean }) {
  const cash = value.account_type !== "margin"
  const portfolio = !cash && value.margin === "portfolio"
  return <fieldset className="space-y-2" disabled={disabled}>
    <legend className="mb-1 text-[11px] font-medium uppercase tracking-wide text-muted">Margin</legend>
    <div className="grid grid-cols-2 gap-2">
      <label className="block space-y-1 text-sm">
        <span>Account type</span>
        <select className="trade-input w-full" value={value.account_type}
          onChange={(event) => onChange({ ...value, account_type: event.target.value as AccountType })}>
          <option value="margin">Margin</option>
          <option value="cash">Cash</option>
          <option value="ira">IRA</option>
        </select>
      </label>
      <label className="block space-y-1 text-sm">
        <span>Margin</span>
        <select className="trade-input w-full" value={cash ? "strategy" : value.margin} disabled={cash}
          onChange={(event) => onChange({ ...value, margin: event.target.value as MarginChoice["margin"] })}>
          <option value="strategy">Strategy (Reg T)</option>
          <option value="portfolio">Portfolio</option>
        </select>
      </label>
      <label className="block space-y-1 text-sm">
        <span>House margin %</span>
        <input className="trade-input w-full" type="number" min={0} max={400} step={1} value={value.house_margin_percent}
          onChange={(event) => onChange({ ...value, house_margin_percent: whole(event.target.value, 400) })} />
      </label>
      <label className="block space-y-1 text-sm">
        <span>Portfolio IV shock (points)</span>
        <input className="trade-input w-full" type="number" min={0} max={50} step={1} value={portfolio ? value.pm_vol_shock : 0} disabled={!portfolio}
          onChange={(event) => onChange({ ...value, pm_vol_shock: whole(event.target.value, 50) })} />
      </label>
    </div>
    <p className="text-xs text-muted">{value.account_type === "cash"
      ? "Cash: calls only against 100 shares each, short puts secured with their strike, no spreads netted and no short sales."
      : value.account_type === "ira"
        ? "IRA: spreads net to what they can lose and long calls cover short calls, but short puts are cash-secured, no straddles pair and nothing is sold naked or short."
        : portfolio
          ? "Portfolio margin: each underlying holds its largest loss over the price scan, at implied volatility moved up and down by the shock; long options count as collateral."
          : "Strategy margin: spreads net to their width, covered calls hold nothing, and naked shorts hold Reg T's 20% of spot."}
      {value.house_margin_percent ? ` House margin adds ${value.house_margin_percent}% to ${portfolio ? "the scan" : "naked and short-sale requirements"}.` : ""}</p>
  </fieldset>
}
