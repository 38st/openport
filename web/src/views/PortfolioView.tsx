import { useRef, useState, type ReactNode } from "react"
import { api } from "../api/client"
import { useLive } from "../api/live"
import { useRefreshTrading, useTradingQueries, useTradingSession } from "../api/trading"
import type { Fill, Order, Position, Risk, TradingStatus } from "../api/trading-types"
import { KillSwitch } from "../components/KillSwitch"
import { LimitsEditor } from "../components/LimitsEditor"
import { RiskPanel } from "../components/RiskPanel"
import { ScenarioGrid } from "../components/ScenarioGrid"
import { TradingError, WriteAccess, writeBlocked } from "../components/TradingControls"
import { Empty, Panel, Stat } from "../components/ui"
import { fixed } from "../lib/format"
import { timestampET } from "../lib/freshness"
import { formatMoney, paperNotice } from "../lib/trading"
import { useWriteToken } from "../lib/write-token"

function TradingTable({ label, headers, children }: { label: string; headers: string[]; children: ReactNode }) {
  return <div className="max-w-full overflow-x-auto" tabIndex={0} role="region" aria-label={label}>
    <table className="w-full text-right text-xs tabular whitespace-nowrap">
      <thead className="text-muted"><tr>{headers.map((header, i) => <th key={header} scope="col" className={`px-2 py-2 font-normal ${i === 0 ? "text-left" : ""}`}>{header}</th>)}</tr></thead>
      <tbody className="[&_td]:px-2 [&_td]:py-2 [&_tr]:border-t [&_tr]:border-border/40 [&_td:first-child]:text-left">{children}</tbody>
    </table>
  </div>
}
function Positions({ positions }: { positions: Position[] }) {
  if (!positions.length) return <p className="text-sm text-muted">No positions. Place a paper order from a chain bid or ask.</p>
  return <TradingTable label="Positions" headers={["Symbol", "Qty", "Avg price", "Mark / age", "Market value", "Unrealised", "Dollar delta", "Vega", "Theta"]}>
    {positions.map((position) => <tr key={position.symbol}>
      <td><div>{position.symbol}</div><div className="mt-1 text-[11px] text-muted">{position.expiry} {position.settlement} · {position.strike} {position.type}</div>{position.awaiting_settlement && <span className="mt-1 inline-block rounded-full border border-warn px-2 py-0.5 text-[10px] text-warn">Awaiting settlement</span>}</td>
      <td>{position.quantity}</td><td>{formatMoney(position.average_price)}</td>
      <td><div>{formatMoney(position.mark)}</div><div className={`mt-1 text-[11px] ${position.fresh ? "text-muted" : "text-warn"}`}>{position.mark_age_seconds == null ? "Age unavailable" : `${fixed(position.mark_age_seconds, 0)} s old`}{!position.fresh ? " · stale" : ""}</div></td>
      <td>{formatMoney(position.market_value)}</td><td>{formatMoney(position.unrealised)}</td><td>{fixed(position.greeks.dollar_delta, 2)}</td><td>{fixed(position.greeks.vega_dollars, 2)}</td><td>{fixed(position.greeks.theta_dollars, 2)}</td>
    </tr>)}
  </TradingTable>
}
function OpenOrders({ orders, trading }: { orders: Order[]; trading: TradingStatus }) {
  const token = useWriteToken()
  const [pending, setPending] = useState<string | null>(null)
  const [error, setError] = useState<unknown>()
  const [result, setResult] = useState<string | null>(null)
  const busy = useRef(false)
  const refresh = useRefreshTrading()
  const sameSession = useTradingSession()
  async function cancel(id: string) {
    if (busy.current || writeBlocked(trading, token)) return
    busy.current = true; setPending(id); setError(undefined)
    try {
      const response = await api.cancelOrder(id, trading.write)
      if (sameSession()) setResult(`${response.order.symbol}: ${response.order.status}`)
    } catch (failure) { if (sameSession()) setError(failure) }
    finally { busy.current = false; if (sameSession()) setPending(null); void refresh() }
  }
  return <div className="space-y-2">
    <TradingError error={error} />
    {result && <p className="text-xs text-muted" role="status">{result}</p>}
    {!orders.length ? <p className="text-sm text-muted">No open orders.</p> : <TradingTable label="Open orders" headers={["Symbol", "Side / type", "Qty / filled", "Remaining", "Limit", "Status", "Action"]}>
      {orders.map((order) => <tr key={order.id}>
        <td>{order.symbol}</td><td>{order.side} · {order.type} · {order.time_in_force}</td><td>{order.quantity} / {order.filled_quantity}</td><td>{order.remaining_quantity}</td><td>{formatMoney(order.limit_price)}</td>
        <td><span>{order.status === "partially_filled" ? "Partial fill" : order.status}</span>{order.reason && <div title={order.reason.message} className="text-warn">{order.reason.code}</div>}</td>
        <td><button type="button" className="trade-button" aria-label={`Cancel order ${order.id} for ${order.symbol}`} disabled={pending != null || writeBlocked(trading, token)} onClick={() => void cancel(order.id)}>{pending === order.id ? "Cancelling…" : "Cancel"}</button></td>
      </tr>)}
    </TradingTable>}
  </div>
}
function RecentFills({ fills }: { fills: Fill[] }) {
  if (!fills.length) return <p className="text-sm text-muted">No fills yet.</p>
  return <TradingTable label="Recent fills" headers={["Symbol", "Side", "Qty", "Price", "Fee", "Filled at", "Quote time"]}>
    {fills.slice(0, 50).map((fill) => <tr key={fill.id}><td>{fill.symbol}</td><td>{fill.side}</td><td>{fill.quantity}</td><td>{formatMoney(fill.price)}</td><td>{formatMoney(fill.fee)}</td><td>{timestampET(fill.time)}</td><td>{timestampET(fill.quote_time)}</td></tr>)}
  </TradingTable>
}
export function PortfolioView() {
  const { trading, accountScope } = useLive()
  if (!trading) return null
  return <PortfolioAccount key={accountScope} trading={trading} />
}
function PortfolioAccount({ trading }: { trading: TradingStatus }) {
  const { underlyings } = useLive()
  const { portfolio, orders, fills, risk } = useTradingQueries()
  const [editing, setEditing] = useState<Risk | null>(null)
  const refresh = useRefreshTrading()
  const account = portfolio.data
  return <div className="min-w-0 space-y-3">
    <div className="flex flex-wrap items-center justify-between gap-3">
      <div><h1 className="text-sm font-medium">Paper portfolio</h1><p className="mt-1 text-xs text-muted">European cash-settled index options · account version {account?.account_version ?? trading.account_version}</p></div>
      <div className="flex flex-wrap items-center gap-2"><WriteAccess trading={trading} /><button type="button" className="trade-button" onClick={() => void refresh()}>Refresh</button></div>
    </div>
    {underlyings.filter((u) => u.has_tradable_contracts ||
      account?.positions.some((p) => p.underlying === u.symbol && p.quantity !== 0) ||
      orders.data?.orders.some((o) => o.underlying === u.symbol && o.remaining_quantity > 0 &&
        (o.status === "working" || o.status === "partially_filled"))).map((u) => {
      const notice = paperNotice(u.symbol, u)
      return notice && <p key={u.symbol} role="status" className="text-sm text-warn">{notice}</p>
    })}
    {!trading.enabled && <p className="rounded-md border border-warn p-3 text-sm text-warn">{trading.reason ?? "Paper trading is unavailable."}</p>}
    <TradingError error={portfolio.error} />
    {account ? <>
      {!account.valuation_complete && <div role="status" className="rounded-md border border-warn p-3 text-sm text-warn">Valuation incomplete · equity and P&amp;L may omit unpriced positions.{account.quality_flags.length > 0 && <div className="mt-1 break-words text-xs">{account.quality_flags.join(" · ")}</div>}</div>}
      {account.valuation_complete && account.quality_flags.length > 0 && <p className="text-xs text-warn">{account.quality_flags.join(" · ")}</p>}
      <div className="grid min-w-0 grid-cols-2 gap-2 sm:grid-cols-3 xl:grid-cols-7">{([
        ["Equity", account.equity], ["Cash", account.cash], ["Day P&L", account.day_pnl], ["Realised", account.realised], ["Unrealised", account.unrealised], ["Fees", account.fees], ["Start of day", account.start_of_day_equity],
      ] as const).map(([label, value]) => <Stat key={label} label={label} value={<span className="break-all">{formatMoney(value)}</span>} />)}</div>
      <p className="text-[11px] text-muted">Valued {timestampET(account.time)}</p>
      <Panel title="Positions"><Positions positions={account.positions} /></Panel>
    </> : trading.enabled && !portfolio.error ? <Empty>Loading portfolio…</Empty> : null}
    <Panel title="Open orders"><TradingError error={orders.error} />{orders.data ? <OpenOrders orders={orders.data.orders} trading={trading} /> : <p className="text-sm text-muted">{trading.enabled ? "Loading orders…" : "Orders unavailable"}</p>}</Panel>
    <Panel title="Recent fills · latest 50"><TradingError error={fills.error} />{fills.data ? <RecentFills fills={fills.data.fills} /> : <p className="text-sm text-muted">{trading.enabled ? "Loading fills…" : "Fills unavailable"}</p>}</Panel>
    <Panel title="Portfolio risk" actions={<button type="button" className="trade-button" disabled={!risk.data || !trading.enabled} onClick={() => { if (risk.data) setEditing(risk.data) }}>Edit limits</button>}>
      <TradingError error={risk.error} />{risk.data ? <RiskPanel risk={risk.data} /> : <p className="text-sm text-muted">{trading.enabled ? "Loading risk…" : "Risk unavailable"}</p>}
    </Panel>
    {risk.data && <Panel title="Spot × volatility scenarios"><ScenarioGrid scenarios={risk.data.scenarios} /></Panel>}
    <Panel title="Kill switch"><KillSwitch kill={risk.data?.kill ?? { latched: trading.kill_latched, reason: null }} trading={trading} /></Panel>
    {editing && <LimitsEditor initial={editing} trading={trading} onClose={() => setEditing(null)} />}
  </div>
}
