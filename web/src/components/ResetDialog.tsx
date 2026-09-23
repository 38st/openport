import { useRef, useState } from "react"
import { api } from "../api/client"
import { useRefreshTrading, usePlans, useTradingSession } from "../api/trading"
import type { Plan, TradingStatus } from "../api/trading-types"
import { formatMoney } from "../lib/trading"
import { useWriteToken } from "../lib/write-token"
import { Dialog } from "./Dialog"
import { TradingError, WriteAccess, writeBlocked } from "./TradingControls"

export function planFacts(plan: Pick<Plan, "initial_cash" | "rules">): string[] {
  const r = plan.rules
  return [
    `${formatMoney(plan.initial_cash, 0)} starting balance`,
    r.profit_target ? `${formatMoney(r.profit_target, 0)} profit target` : "No profit target",
    r.max_drawdown ? `${formatMoney(r.max_drawdown, 0)} trailing drawdown (${r.drawdown_mode === "intraday" ? "intraday" : "end of day"})` : "No drawdown floor",
    r.buy_only ? "Buy-only, single leg" : "Any strategy",
    ...(r.buying_power ? ["Buying power enforced"] : []),
    ...(r.expiry_cutoff_seconds > 0 ? [`Auto-close ${Math.round(r.expiry_cutoff_seconds / 60)} min before expiry`] : []),
  ]
}

/** Starting an attempt closes every position at its last mark; confirm with a plan choice. */
export function ResetDialog({ trading, attempt, initial, onClose }: { trading: TradingStatus; attempt: number; initial?: string; onClose: () => void }) {
  const plans = usePlans()
  const token = useWriteToken()
  const refresh = useRefreshTrading()
  const sameSession = useTradingSession()
  const [choice, setChoice] = useState(initial ?? "")
  const [pending, setPending] = useState(false)
  const [error, setError] = useState<unknown>()
  const busy = useRef(false)
  const list = plans.data?.plans ?? []
  const selected = list.find((p) => p.id === choice) ?? null
  async function submit() {
    if (!selected || busy.current || writeBlocked(trading, token)) return
    busy.current = true
    setPending(true)
    setError(undefined)
    try {
      await api.resetAccount({ plan: selected.id, reason: `Start ${selected.name}` }, trading.write)
      if (sameSession()) onClose()
    } catch (failure) {
      if (sameSession()) setError(failure)
    } finally {
      busy.current = false
      if (sameSession()) setPending(false)
      void refresh()
    }
  }
  return (
    <Dialog title="Start a new attempt" onClose={onClose}>
      <p className="text-sm text-muted">
        Attempt {attempt + 1} starts fresh: working orders are cancelled, open positions close at their last mark,
        and cash returns to the plan's starting balance. Your trade history is kept.
      </p>
      <WriteAccess trading={trading} />
      <TradingError error={plans.error} />
      <form className="space-y-3" onSubmit={(event) => { event.preventDefault(); void submit() }}>
        <fieldset className="space-y-2" disabled={pending}>
          <legend className="sr-only">Plan</legend>
          {list.map((plan) => (
            <label key={plan.id} className={`block cursor-pointer rounded-md border p-3 ${choice === plan.id ? "border-accent bg-accent/5" : "border-border hover:border-muted"}`}>
              <div className="flex items-start gap-2">
                <input type="radio" name="plan" value={plan.id} checked={choice === plan.id} onChange={() => setChoice(plan.id)} className="mt-1 accent-[var(--accent)]" />
                <div className="min-w-0">
                  <div className="text-sm font-medium">{plan.name}</div>
                  <div className="mt-0.5 text-xs text-muted">{plan.summary}</div>
                  <div className="mt-1 text-[11px] text-faint">{planFacts(plan).join(" · ")}</div>
                </div>
              </div>
            </label>
          ))}
          {plans.isLoading && <p className="text-sm text-muted">Loading plans…</p>}
        </fieldset>
        <TradingError error={error} />
        <button type="submit" className="trade-button" disabled={!selected || pending || writeBlocked(trading, token)}>
          {pending ? "Starting…" : selected ? `Start ${selected.name}` : "Choose a plan"}
        </button>
      </form>
    </Dialog>
  )
}
