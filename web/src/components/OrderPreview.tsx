import { useQuery } from "@tanstack/react-query"
import { useEffect, useState } from "react"
import { api } from "../api/client"
import { useLive } from "../api/live"
import type { NewOrder, OrderPreview, TradingStatus } from "../api/trading-types"
import { formatMoney } from "../lib/trading"
import { useWriteToken } from "../lib/write-token"
import { BreachPanel } from "./BreachPanel"

export function useOrderPreview(order: NewOrder | null, trading: TradingStatus) {
  const { accountScope } = useLive()
  const token = useWriteToken()
  const signature = JSON.stringify(order)
  const [ready, setReady] = useState("")
  useEffect(() => {
    const timer = window.setTimeout(() => setReady(signature), 300)
    return () => window.clearTimeout(timer)
  }, [signature])
  const query = useQuery({
    queryKey: ["trading", accountScope, "preview", trading.account_version, ready, trading.write, token],
    queryFn: () => api.previewOrder(JSON.parse(ready) as NewOrder, trading.write),
    enabled: order != null && signature === ready && trading.enabled,
    retry: false,
  })
  return { data: signature === ready && order ? query.data : undefined, error: signature === ready ? query.error : null,
    loading: order != null && (signature !== ready || query.isFetching) }
}
// exposure_change is the order's change to the book's Greeks, not the book after it.
const greekChanges = [["dollar_delta", "Dollar delta"], ["dollar_gamma_1pct", "Dollar gamma per 1%"], ["vega", "Vega"], ["theta", "Theta"]] as const
function signed(value: number) { return `${value > 0 ? "+" : ""}${value.toFixed(2)}` }
function units(value: number | null | undefined) { return value == null ? "Unavailable" : `${value} ${value === 1 ? "unit" : "units"}` }
export function OrderPreviewPanel({ preview, onSize, disabled = false }: {
  preview: { data?: OrderPreview; error: unknown; loading: boolean }; onSize: (size: number) => void; disabled?: boolean
}) {
  const p = preview.data
  // Without a plan or soft floor, size follows buying power and the limits alone.
  const floorless = p != null && p.max_units_floor === null && p.breach.room == null && p.breach.soft_room == null
  return <section aria-label="Order preview" className="space-y-2 rounded-md border border-border p-3 text-xs">
    <div className="flex items-center justify-between gap-2"><span className="font-medium">Simulated order preview</span>
      <button type="button" className="trade-button" disabled={disabled || preview.loading || !p || p.max_units == null || p.max_units < 1 || !!preview.error}
        onClick={() => { if (p?.max_units) onSize(p.max_units) }}>{floorless ? "Size to buying power" : "Size to floor"}</button></div>
    {preview.loading ? <p className="text-muted">Checking order…</p>
      : preview.error ? <p role="status" className="text-warn">Preview failed. Buying power and floor risk are unavailable. You can still submit for the server's checks.</p>
      : !p ? <p className="text-muted">Complete the order to preview its risk.</p> : <>
        <dl className="grid grid-cols-2 gap-2 tabular">
          <dt className="text-muted">Buying power after</dt><dd className="text-right">{formatMoney(p.buying_power.after)}</dd>
          <dt className="text-muted">{p.max_loss_basis === "scenario_grid" ? "Scenario-grid loss" : "Max loss at expiry"}</dt><dd className="text-right">{formatMoney(p.max_loss)}</dd>
          <dt className="text-muted">Room after this order</dt><dd className="text-right">{formatMoney(p.breach.room)}</dd>
          {floorless ? <><dt className="text-muted">Size within buying power and limits</dt><dd className="text-right">{units(p.max_units)}</dd></>
            : p.max_units_buying_power === undefined ? <><dt className="text-muted">Size at 50% of floor room</dt><dd className="text-right">{units(p.max_units)}</dd></>
            : <>
              <dt className="text-muted">Fits buying power</dt><dd className="text-right">{units(p.max_units_buying_power)}</dd>
              <dt className="text-muted">Fits 50% of floor room</dt><dd className="text-right">{units(p.max_units_floor)}</dd>
              <dt className="text-muted">Size that fits both</dt><dd className="text-right">{units(p.max_units)}</dd>
            </>}
        </dl>
        {p.decision !== "ok" && <p role="status" className="text-warn">{p.decision}: {p.reason?.message}</p>}
        {(p.breaches_floor || p.breaches_soft_floor) && <p role="alert" className="font-medium text-danger">This order could breach {p.breaches_floor ? "the plan floor" : "your soft floor"}.</p>}
        <details><summary className="cursor-pointer text-muted">After this order · breach risk, and the change in Greeks</summary>
          <BreachPanel breach={p.breach} />
          {p.exposure_change && <section aria-label="Change in Greeks" className="mt-2">
            <p className="text-muted">Change from this order</p>
            <dl className="mt-1 grid grid-cols-2 gap-1 tabular">{greekChanges.map(([key, label]) =>
              <div key={key}><dt className="text-muted">{label}</dt><dd>{signed(p.exposure_change![key])}</dd></div>)}</dl>
          </section>}
        </details>
        <p className="text-faint">Full-size projection including fees. {floorless ? "Size uses buying power and limits: the account has no plan or soft floor." : "Size uses buying power, limits and 50% of the nearer floor's room."} Sizing is unavailable when the order is refused at any size or its loss cannot be projected. Scenario losses are model estimates.</p>
      </>}
  </section>
}
