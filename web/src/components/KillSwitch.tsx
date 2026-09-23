import { useRef, useState } from "react"
import { api } from "../api/client"
import { useRefreshTrading, useTradingSession } from "../api/trading"
import type { KillState, TradingStatus } from "../api/trading-types"
import { useWriteToken } from "../lib/write-token"
import { Dialog } from "./Dialog"
import { TradingError, writeBlocked } from "./TradingControls"

export function KillSwitch({ kill, trading }: { kill: KillState; trading: TradingStatus }) {
  const [reason, setReason] = useState("")
  const [action, setAction] = useState<"trip" | "reset" | null>(null)
  const [error, setError] = useState<unknown>()
  const [pending, setPending] = useState(false)
  const [result, setResult] = useState<string | null>(null)
  const busy = useRef(false)
  const token = useWriteToken()
  const refresh = useRefreshTrading()
  const sameSession = useTradingSession()
  const blocked = writeBlocked(trading, token)
  async function confirm() {
    if (!action || !reason.trim() || busy.current || blocked) return
    busy.current = true; setPending(true); setError(undefined)
    try {
      const response = await api.setKill(action, reason.trim(), trading.write)
      if (sameSession()) {
        setResult(`${response.kill.latched ? "Kill switch latched" : "Kill switch reset"}. ${response.cancelled_orders.length} orders cancelled.`)
        setAction(null); setReason("")
      }
    } catch (failure) { if (sameSession()) setError(failure) }
    finally { busy.current = false; if (sameSession()) setPending(false); void refresh() }
  }
  return <div className="space-y-3">
    <div className={`rounded-md border p-3 text-sm ${kill.latched ? "border-danger text-danger" : "border-border text-muted"}`}>
      <strong>{kill.latched ? "LATCHED · new orders blocked" : "Armed · trading permitted"}</strong>
      <p className="mt-1 break-words">{kill.reason ?? (kill.latched ? "No reason provided" : "Trip to cancel open orders and block new orders.")}</p>
    </div>
    <label className="trade-label">Reason<input className="trade-input" value={reason} onChange={(e) => setReason(e.target.value)} placeholder={kill.latched ? "Why is it safe to resume?" : "Why are you stopping trading?"} /></label>
    <button className="trade-button" type="button" disabled={blocked || !reason.trim()} onClick={() => { setError(undefined); setAction(kill.latched ? "reset" : "trip") }}>{kill.latched ? "Reset kill switch…" : "Trip kill switch…"}</button>
    {result && <p role="status" className="text-xs text-muted">{result}</p>}
    {action && <Dialog title={action === "trip" ? "Confirm kill switch" : "Confirm trading reset"} onClose={() => { if (!pending) setAction(null) }}>
      <p className="text-sm">{action === "trip" ? "Cancel all open orders and latch the kill switch? Existing positions will remain open." : "Clear the latched kill switch and allow new orders? Risk limits will still apply."}</p>
      <p className="break-words text-sm text-muted">Reason: {reason}</p>
      <TradingError error={error} />
      <div className="flex gap-2"><button className="trade-button" disabled={pending || blocked} onClick={() => void confirm()}>{pending ? "Applying…" : action === "trip" ? "Confirm trip" : "Confirm reset"}</button><button className="trade-button" disabled={pending} onClick={() => setAction(null)}>Back</button></div>
    </Dialog>}
  </div>
}
