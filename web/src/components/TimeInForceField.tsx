import type { TimeInForce } from "../api/trading-types"
import { Segmented } from "./ui"

export function goodTillTimestamp(value: string): string | undefined {
  if (!value) return undefined
  const time = Date.parse(`${value}Z`)
  return Number.isFinite(time) ? new Date(time).toISOString() : undefined
}

export function TimeInForceField({ value, onChange, market = false, conditional = false, goodTill, setGoodTill }: {
  value: TimeInForce; onChange: (value: TimeInForce) => void; market?: boolean; conditional?: boolean
  goodTill: string; setGoodTill: (value: string) => void
}) {
  const options: { value: TimeInForce; label: string }[] = [
    ...(!market ? [{ value: "day" as const, label: "Day" }, { value: "gtc" as const, label: "GTC" }] : []),
    { value: "ioc", label: "IOC" },
    ...(!market || conditional ? [{ value: "exto" as const, label: "EXTO" }, { value: "gtc_exto" as const, label: "GTC + EXTO" }, { value: "gtd" as const, label: "GTD" }] : []),
  ]
  return <div className="trade-label min-w-0 [&>[role=radiogroup]]:flex-wrap">Time in force
    <Segmented label="Time in force" value={value} onChange={onChange} options={options} />
    {value === "gtd" && <label className="trade-label">Good until (UTC)
      <input className="trade-input" type="datetime-local" value={goodTill} onChange={(e) => setGoodTill(e.target.value)} required />
      <span className="text-[11px] text-muted">Future market time, within 366 days. GTD trades regular hours.</span>
    </label>}
    {(value === "exto" || value === "gtc_exto") && <span className="text-[11px] text-muted">
      {value === "exto" ? "Works through this trading date’s last session." : "Works in every product session until expiry or auto-close."}
      {" "}Exits inherit all sessions. Stop-limit keeps your price; a stop-market takes displayed liquidity at the touch outside regular hours.
    </span>}
  </div>
}
