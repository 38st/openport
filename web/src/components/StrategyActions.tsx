import { useQueries, useQuery } from "@tanstack/react-query"
import { useState } from "react"
import { api } from "../api/client"
import { useLive } from "../api/live"
import type { TradingStatus } from "../api/trading-types"
import type { Chain } from "../api/types"
import { days, expiryLabel, fixed } from "../lib/format"
import { closingPlan, rollPlan, type StrategyGroup } from "../lib/positions"
import type { StrategyLeg } from "../lib/strategy"
import { formatMoney, signedMoney } from "../lib/trading"
import { Dialog } from "./Dialog"
import { StrategyTicket } from "./StrategyTicket"
import { TradingError } from "./TradingControls"
import { Badge, toneOf, toneText } from "./ui"

/** Every chain the legs trade in, all strikes, for their live quotes. */
function useChains(underlying: string, ids: readonly string[]) {
  const { version } = useLive()
  const results = useQueries({ queries: ids.map((id) => ({
    queryKey: ["chain", underlying, id, 0, version(underlying)],
    queryFn: ({ signal }: { signal: AbortSignal }) => api.chain(underlying, id, 0, signal),
  })) })
  const chains = results.map((r) => r.data).filter((c): c is Chain => c != null)
  return { chains, ready: chains.length === ids.length, error: results.find((r) => r.error)?.error }
}
const withQuotes = (legs: StrategyLeg[], chains: readonly Chain[]) => legs.map((leg) => ({ ...leg,
  quote: chains.find((c) => c.expiry.id === leg.expiry)?.strikes.find((r) => r.strike === leg.strike)?.[leg.type] ?? leg.quote }))

/** One multi-leg order that closes held positions together, each leg reversed. */
export function CloseStrategyDialog({ plan, underlying, title = "Close together", trading, onClose }: {
  plan: { legs: StrategyLeg[]; units: number }; underlying: string; title?: string; trading: TradingStatus; onClose: () => void
}) {
  const ids = [...new Set(plan.legs.map((l) => l.expiry))]
  const { chains, ready, error } = useChains(underlying, ids)
  const [edited, setEdited] = useState<StrategyLeg[] | null>(null)
  if (!ready) return error ? <Dialog title={title} onClose={onClose}><TradingError error={error} /></Dialog> : null
  return <StrategyTicket title={title} closing legs={withQuotes(edited ?? plan.legs, chains)} onLegs={setEdited} expiries={chains.map((c) => c.expiry)}
    underlying={underlying} spot={chains[0]?.spot} trading={trading} variant="dialog" units={plan.units} onClose={onClose} />
}

/** Close a strategy and open it again at a later expiry, as one order. */
export function RollDialog({ group, trading, onClose }: { group: StrategyGroup; trading: TradingStatus; onClose: () => void }) {
  const { version } = useLive()
  const summary = useQuery({
    queryKey: ["summary", group.underlying, version(group.underlying)],
    queryFn: ({ signal }) => api.summary(group.underlying, signal),
  })
  const later = (summary.data?.expiries ?? []).filter((e) => e.id > group.expiry)
  const [choice, setChoice] = useState<string | null>(null)
  const target = choice ?? later[0]?.id ?? null
  const { chains, ready, error } = useChains(group.underlying, target ? [group.expiry, target] : [group.expiry])
  const [edited, setEdited] = useState<{ target: string; legs: StrategyLeg[] } | null>(null)
  const targetChain = chains.find((c) => c.expiry.id === target)
  const plan = targetChain ? rollPlan(group, { id: targetChain.expiry.id, strikes: targetChain.strikes }) : null
  const title = `Roll ${group.label.toLowerCase()}`
  return (
    <Dialog title={title} onClose={onClose}>
      <div className="text-sm">
        <div className="font-medium">{group.title}</div>
        <div className="text-xs text-muted">{group.units} unit{group.units === 1 ? "" : "s"}, opened at {group.cost == null ? "—" : `${formatMoney(Math.abs(group.cost).toFixed(2))} ${group.cost < 0 ? "credit" : "debit"}`}</div>
      </div>
      <label className="flex items-center gap-2 text-sm">
        <span className="text-muted">Roll to</span>
        <select className="trade-input !w-auto !py-1" value={target ?? ""} disabled={!later.length}
          onChange={(e) => { setChoice(e.target.value); setEdited(null) }}>
          {later.map((e) => <option key={e.id} value={e.id}>{expiryLabel(e.id, true)} · {days(e.days)}</option>)}
        </select>
      </label>
      <TradingError error={summary.error ?? error} />
      {summary.data && !later.length && <p className="text-sm text-muted">No later expiry is listed.</p>}
      {plan && "reason" in plan && <p className="text-sm text-warn">{plan.reason}</p>}
      {ready && plan && "legs" in plan && target && (
        <StrategyTicket variant="bare" roll title={title} legs={withQuotes(edited?.target === target ? edited.legs : plan.legs, chains)}
          onLegs={(legs) => setEdited({ target, legs })} expiries={chains.map((c) => c.expiry)} underlying={group.underlying}
          spot={chains[0]?.spot} trading={trading} units={plan.units} onClose={onClose} />
      )}
    </Dialog>
  )
}

/** Held strategies with their net value, P&L and Greeks, and one-click close and roll. */
export function Strategies({ groups, trading }: { groups: readonly StrategyGroup[]; trading: TradingStatus }) {
  const [closing, setClosing] = useState<StrategyGroup | null>(null)
  const [rolling, setRolling] = useState<StrategyGroup | null>(null)
  const net = (value: number | null) => value == null ? "—" : `${formatMoney(Math.abs(value).toFixed(2))} ${value < 0 ? "cr" : "db"}`
  return <>
    <div className="max-w-full overflow-x-auto" tabIndex={0} role="region" aria-label="Strategies">
      <table className="w-full text-right text-xs tabular whitespace-nowrap">
        <thead className="text-muted"><tr>{["Strategy", "Units", "Opened", "Now", "Open P&L", "Max profit", "Max loss", "Dollar delta", "Theta", "To expiry", ""].map((h, i) =>
          <th key={h} scope="col" className={`px-2 py-2 text-[11px] font-normal uppercase tracking-wide ${i === 0 ? "text-left" : ""}`}>{h}</th>)}</tr></thead>
        <tbody className="[&_td]:px-2 [&_td]:py-2 [&_tr]:border-t [&_tr]:border-border/40">
          {groups.map((group) => {
            const left = group.expires != null && Number.isFinite(group.expires) ? (group.expires - Date.now()) / 86_400_000 : null
            return <tr key={group.order.id}>
              <td className="text-left">
                <div className="flex items-center gap-1.5 font-medium">{group.label} <Badge tone="accent">{group.legs.length} legs</Badge></div>
                <div className="mt-0.5 text-[11px] text-faint">{group.title} · #{group.order.id}</div>
              </td>
              <td>{group.units}</td>
              <td>{net(group.cost)}</td>
              <td>{net(group.value)}</td>
              <td className={toneText[toneOf(group.pnl)]}>{group.pnl == null ? "—" : signedMoney(group.pnl.toFixed(2))}</td>
              <td className="text-bullish">{group.profile ? group.profile.maxProfit == null ? "Unlimited" : formatMoney(group.profile.maxProfit.toFixed(2)) : "—"}</td>
              <td className="text-bearish">{group.profile ? group.profile.maxLoss == null ? "Unlimited" : formatMoney(group.profile.maxLoss.toFixed(2)) : "—"}</td>
              <td>{fixed(group.greeks.dollar_delta, 2)}</td>
              <td>{fixed(group.greeks.theta_dollars, 2)}</td>
              <td>{left == null ? "—" : days(Math.max(0, left))}</td>
              <td><div className="flex justify-end gap-1">
                <button type="button" className="trade-button" disabled={!trading.enabled} aria-label={`Close ${group.label} ${group.title}`} onClick={() => setClosing(group)}>Close</button>
                <button type="button" className="trade-button" disabled={!trading.enabled || group.legs.length > 2} aria-label={`Roll ${group.label} ${group.title}`}
                  title={group.legs.length > 2 ? "Rolls take up to two legs as one order" : "Close and reopen at a later expiry"} onClick={() => setRolling(group)}>Roll</button>
              </div></td>
            </tr>
          })}
        </tbody>
      </table>
    </div>
    <p className="mt-2 text-[11px] text-muted">A strategy is the positions one multi-leg order opened, while they are still held together. P&L is before fees; max profit and loss are at expiry.</p>
    {closing && <CloseStrategyDialog plan={closingPlan(closing)} underlying={closing.underlying} title={`Close ${closing.label.toLowerCase()}`}
      trading={trading} onClose={() => setClosing(null)} />}
    {rolling && <RollDialog group={rolling} trading={trading} onClose={() => setRolling(null)} />}
  </>
}
