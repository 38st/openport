import { useRef, useState } from "react"
import { api, ApiError } from "../api/client"
import { useRefreshTrading, useTradingSession } from "../api/trading"
import type { Limits, Risk, TradingStatus } from "../api/trading-types"
import { validMoney } from "../lib/trading"
import { useWriteToken } from "../lib/write-token"
import { Dialog } from "./Dialog"
import { TradingError, WriteAccess, writeBlocked } from "./TradingControls"

function draftOf(limits: Limits) {
  return {
    max_order_contracts: String(limits.max_order_contracts), price_band_absolute: limits.price_band_absolute,
    price_band_relative: String(limits.price_band_relative), aggregate_delta: String(limits.aggregate.dollar_delta),
    aggregate_vega: String(limits.aggregate.vega), underlying_delta: String(limits.per_underlying.dollar_delta),
    underlying_vega: String(limits.per_underlying.vega), max_daily_loss: limits.max_daily_loss,
    max_quote_age_seconds: String(limits.max_quote_age_seconds), max_valuation_age_seconds: String(limits.max_valuation_age_seconds),
  }
}
const fields: { key: keyof ReturnType<typeof draftOf>; label: string; money?: boolean; integer?: boolean }[] = [
  { key: "max_order_contracts", label: "Max contracts / order", integer: true },
  { key: "price_band_absolute", label: "Absolute price band ($)", money: true },
  { key: "price_band_relative", label: "Relative price band (ratio)" },
  { key: "aggregate_delta", label: "Aggregate dollar delta" }, { key: "aggregate_vega", label: "Aggregate vega" },
  { key: "underlying_delta", label: "Per-underlying dollar delta" }, { key: "underlying_vega", label: "Per-underlying vega" },
  { key: "max_daily_loss", label: "Max daily loss ($)", money: true },
  { key: "max_quote_age_seconds", label: "Max quote age (seconds)", integer: true },
  { key: "max_valuation_age_seconds", label: "Max valuation age (seconds)", integer: true },
]
export function LimitsEditor({ initial, trading, onClose }: { initial: Risk; trading: TradingStatus; onClose: () => void }) {
  const [revision, setRevision] = useState(initial.limits_revision)
  const [draft, setDraft] = useState(() => draftOf(initial.limits))
  const [error, setError] = useState<unknown>()
  const [pending, setPending] = useState(false)
  const busy = useRef(false)
  const token = useWriteToken()
  const refresh = useRefreshTrading()
  const sameSession = useTradingSession()
  const conflict = error instanceof ApiError && error.code === "LIMITS_REVISION"
  const valid = fields.every(({ key, money, integer }) => money ? validMoney(draft[key]) : draft[key].trim() !== "" && Number.isFinite(Number(draft[key])) && Number(draft[key]) >= 0 && (!integer || Number.isSafeInteger(Number(draft[key]))))
  async function save() {
    if (busy.current || !valid || conflict || writeBlocked(trading, token)) return
    busy.current = true; setPending(true); setError(undefined)
    const limits: Limits = {
      max_order_contracts: Number(draft.max_order_contracts), price_band_absolute: draft.price_band_absolute,
      price_band_relative: Number(draft.price_band_relative), max_daily_loss: draft.max_daily_loss,
      aggregate: { dollar_delta: Number(draft.aggregate_delta), vega: Number(draft.aggregate_vega) },
      per_underlying: { dollar_delta: Number(draft.underlying_delta), vega: Number(draft.underlying_vega) },
      max_quote_age_seconds: Number(draft.max_quote_age_seconds), max_valuation_age_seconds: Number(draft.max_valuation_age_seconds),
    }
    try { await api.updateLimits(revision, limits, trading.write); if (sameSession()) onClose() }
    catch (failure) { if (sameSession()) setError(failure) }
    finally { busy.current = false; if (sameSession()) setPending(false); void refresh() }
  }
  async function reload() {
    if (busy.current) return
    busy.current = true; setPending(true)
    try {
      const latest = await api.risk()
      if (sameSession()) { setRevision(latest.limits_revision); setDraft(draftOf(latest.limits)); setError(undefined) }
    } catch (failure) { if (sameSession()) setError(failure) }
    finally { busy.current = false; if (sameSession()) setPending(false) }
  }
  return <Dialog title="Edit risk limits" onClose={onClose}>
    <p className="text-xs text-muted">Editing revision {revision}. A newer server revision must be reloaded before saving.</p>
    <WriteAccess trading={trading} />
    <form className="space-y-4" onSubmit={(event) => { event.preventDefault(); void save() }}>
      <fieldset disabled={pending} className="grid min-w-0 grid-cols-2 gap-3"><legend className="sr-only">Limits</legend>{fields.map(({ key, label, integer }) => <label key={key} className="trade-label">{label}<input className="trade-input" inputMode={integer ? "numeric" : "decimal"} value={draft[key]} onChange={(e) => setDraft({ ...draft, [key]: e.target.value })} required /></label>)}</fieldset>
      <TradingError error={error} />
      {conflict && <div className="space-y-2 text-sm"><p>The limits changed elsewhere. Reload the current limits, review your changes and save again.</p><button type="button" className="trade-button" disabled={pending} onClick={() => void reload()}>Reload current limits (replace edits)</button></div>}
      <button type="submit" className="trade-button" disabled={!valid || pending || conflict || writeBlocked(trading, token)}>{pending ? "Saving…" : "Save limits"}</button>
    </form>
  </Dialog>
}
