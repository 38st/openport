import { useRef, useState } from "react"
import { api } from "../api/client"
import { useLive } from "../api/live"
import { usePlans } from "../api/trading"
import type { TradingStatus } from "../api/trading-types"
import { offeredPlans } from "../lib/payouts"
import { useWriteToken } from "../lib/write-token"
import { Dialog } from "./Dialog"
import { planFacts } from "./ResetDialog"
import { TradingError, WriteAccess, writeBlocked } from "./TradingControls"

/**
 * Each account is its own evaluation or practice book on the same market: its own
 * journal, rules, orders and positions. The switcher picks the one the terminal acts on.
 */
export function AccountSwitcher() {
  const { accounts, account, switchAccount, trading } = useLive()
  const [creating, setCreating] = useState(false)
  if (!trading || !accounts.length) return null
  return (
    <div className="space-y-1">
      <label className="block text-[10px] uppercase tracking-wide text-muted" htmlFor="account-switcher">Account</label>
      <select id="account-switcher" className="trade-input !py-1 text-xs" value={account}
        onChange={(event) => { if (event.target.value === "+new") setCreating(true); else switchAccount(event.target.value) }}>
        {accounts.map((a) => (
          <option key={a.id} value={a.id}>{a.name}{a.trading.plan ? ` · ${a.trading.plan}` : ""}{a.trading.enabled ? "" : " (unavailable)"}</option>
        ))}
        <option value="+new">New account…</option>
      </select>
      {creating && <NewAccountDialog trading={trading} onClose={() => setCreating(false)}
        onCreated={(id) => { setCreating(false); switchAccount(id) }} />}
    </div>
  )
}

/** Name an account and pick its plan; funded plans unlock from a passed evaluation in an existing account. */
export function NewAccountDialog({ trading, onClose, onCreated }: { trading: TradingStatus; onClose: () => void; onCreated: (id: string) => void }) {
  const { accounts } = useLive()
  const plans = usePlans()
  const token = useWriteToken()
  const [name, setName] = useState(`Account ${accounts.length + 1}`)
  const [choice, setChoice] = useState("")
  const [pending, setPending] = useState(false)
  const [error, setError] = useState<unknown>()
  const busy = useRef(false)
  const list = offeredPlans(plans.data?.plans ?? []).filter((plan) => plan.rules.phase !== "funded")
  const selected = list.find((plan) => plan.id === choice) ?? null
  const trimmed = name.trim()
  async function submit() {
    if (!selected || !trimmed || busy.current || writeBlocked(trading, token)) return
    busy.current = true
    setPending(true)
    setError(undefined)
    try {
      const response = await api.createAccount({ name: trimmed, plan: selected.id }, trading.write)
      onCreated(response.account.id)
    } catch (failure) {
      setError(failure)
    } finally {
      busy.current = false
      setPending(false)
    }
  }
  return (
    <Dialog title="New account" onClose={onClose}>
      <p className="text-sm text-muted">
        A new account trades the same market with its own journal, rules, orders and positions. The others keep
        running: their orders still work and their rules still apply.
      </p>
      <WriteAccess trading={trading} />
      <TradingError error={plans.error} />
      <form className="space-y-3" onSubmit={(event) => { event.preventDefault(); void submit() }}>
        <label className="block space-y-1 text-sm">
          <span className="text-muted">Name</span>
          <input className="trade-input" value={name} maxLength={64} onChange={(event) => setName(event.target.value)} aria-label="Account name" />
        </label>
        <fieldset className="space-y-2" disabled={pending}>
          <legend className="mb-1 text-[11px] font-medium uppercase tracking-wide text-muted">Plan</legend>
          {list.map((plan) => (
            <label key={plan.id} className={`block cursor-pointer rounded-md border p-3 ${choice === plan.id ? "border-accent bg-accent/5" : "border-border hover:border-muted"}`}>
              <div className="flex items-start gap-2">
                <input type="radio" name="plan" value={plan.id} checked={choice === plan.id} onChange={() => setChoice(plan.id)}
                  className="mt-1 accent-[var(--accent)]" />
                <div className="min-w-0">
                  <div className="text-sm font-medium">{plan.name}</div>
                  <div className="mt-0.5 text-xs text-muted">{plan.summary}</div>
                  <div className="mt-1 text-[11px] text-faint">{planFacts(plan).join(" · ")}</div>
                </div>
              </div>
            </label>
          ))}
        </fieldset>
        {plans.isLoading && <p className="text-sm text-muted">Loading plans…</p>}
        <TradingError error={error} />
        <button type="submit" className="trade-button" disabled={!selected || !trimmed || pending || writeBlocked(trading, token)}>
          {pending ? "Creating…" : selected ? `Create ${trimmed || "account"}` : "Choose a plan"}
        </button>
      </form>
    </Dialog>
  )
}
