import { useMemo, useRef, useState } from "react"
import { api } from "../api/client"
import { useLive } from "../api/live"
import { useAllOrders, useFills, useRefreshTrading, useTradingSession } from "../api/trading"
import type { Fill, Order, TradingStatus } from "../api/trading-types"
import { TradingError, WriteAccess, writeBlocked } from "../components/TradingControls"
import { Badge, Empty, PageHeader, Panel, Segmented, type Tone } from "../components/ui"
import { newYorkDate, osiLabel } from "../lib/journal"
import { describeTrigger } from "../lib/ticket"
import { formatMoney } from "../lib/trading"
import { useWriteToken } from "../lib/write-token"
import { Table } from "./PositionsView"

type Tab = "working" | "all" | "fills"
const statusTone: Record<Order["status"], Tone> = {
  working: "accent", partially_filled: "accent", filled: "positive", cancelled: "neutral", rejected: "negative", armed: "warn",
}
const statusLabel: Record<Order["status"], string> = {
  working: "Working", partially_filled: "Partial", filled: "Filled", cancelled: "Cancelled", rejected: "Rejected", armed: "Armed",
}
const timeFormat = new Intl.DateTimeFormat("en-US", { timeZone: "America/New_York", hour: "numeric", minute: "2-digit", second: "2-digit" })
const dayFormat = new Intl.DateTimeFormat("en-US", { timeZone: "America/New_York", weekday: "short", month: "short", day: "numeric", year: "numeric" })
export const open = (order: Order) => order.status === "working" || order.status === "partially_filled" || order.status === "armed"

/** Newest first, split into New York trading days for day headers. */
export function groupByDay<T>(items: readonly T[], time: (item: T) => string): { day: string; label: string; items: T[] }[] {
  const groups: { day: string; label: string; items: T[] }[] = []
  for (const item of items) {
    const iso = time(item)
    const day = newYorkDate(iso)?.date ?? "unknown"
    let group = groups[groups.length - 1]
    if (!group || group.day !== day) {
      group = { day, label: Number.isFinite(Date.parse(iso)) ? dayFormat.format(Date.parse(iso)) : "Unknown date", items: [] }
      groups.push(group)
    }
    group.items.push(item)
  }
  return groups
}

export function OrdersView() {
  const { trading, accountScope } = useLive()
  if (!trading) return null
  return <OrdersAccount key={accountScope} trading={trading} />
}

function OrdersAccount({ trading }: { trading: TradingStatus }) {
  const orders = useAllOrders()
  const fills = useFills()
  const [tab, setTab] = useState<Tab>("working")
  const [search, setSearch] = useState("")
  const [side, setSide] = useState<"all" | "buy" | "sell">("all")
  const [status, setStatus] = useState<"all" | Order["status"]>("all")
  const [origin, setOrigin] = useState<"all" | "user" | "system">("all")
  const all = orders.data?.orders ?? []
  const working = all.filter(open)
  const query = search.trim().toLowerCase()
  const visible = useMemo(() => (tab === "working" ? working : all).filter((o) =>
    (side === "all" || o.side === side) && (status === "all" || o.status === status) &&
    (origin === "all" || (o.origin ?? "user") === origin) &&
    (!query || [o.symbol, o.id, o.client_order_id, osiLabel(o.symbol, o.underlying), o.type, o.side].some((v) => v.toLowerCase().includes(query)))),
  [tab, working, all, side, status, origin, query])
  const fillRows = (fills.data?.fills ?? []).filter((f) => (side === "all" || f.side === side) &&
    (!query || [f.symbol, f.order_id, osiLabel(f.symbol, f.underlying)].some((v) => v.toLowerCase().includes(query))))

  return <div className="min-w-0 space-y-4">
    <PageHeader title="Orders" subtitle="Working orders, order history and fills">
      <WriteAccess trading={trading} />
    </PageHeader>
    <div className="flex flex-wrap items-center gap-2">
      <Segmented label="Order view" value={tab} onChange={setTab} options={[
        { value: "working", label: `Working · ${working.length}` }, { value: "all", label: `All · ${all.length}` }, { value: "fills", label: `Fills · ${fills.data?.fills.length ?? 0}` }]} />
      <input type="search" aria-label="Search orders" placeholder="Search symbol, order ID, type…" value={search} onChange={(e) => setSearch(e.target.value)}
        className="trade-input min-w-48 flex-1 !py-1 sm:max-w-80" />
      <select aria-label="Side filter" className="trade-input !w-auto !py-1" value={side} onChange={(e) => setSide(e.target.value as typeof side)}>
        <option value="all">All sides</option><option value="buy">Buy</option><option value="sell">Sell</option>
      </select>
      {tab !== "fills" && <>
        <select aria-label="Status filter" className="trade-input !w-auto !py-1" value={status} onChange={(e) => setStatus(e.target.value as typeof status)}>
          <option value="all">All statuses</option>
          {(Object.keys(statusLabel) as Order["status"][]).map((s) => <option key={s} value={s}>{statusLabel[s]}</option>)}
        </select>
        <select aria-label="Origin filter" className="trade-input !w-auto !py-1" value={origin} onChange={(e) => setOrigin(e.target.value as typeof origin)}>
          <option value="all">All origins</option><option value="user">Mine</option><option value="system">System</option>
        </select>
      </>}
    </div>
    <TradingError error={tab === "fills" ? fills.error : orders.error} />
    {tab === "fills"
      ? <Panel title="Fills">{fills.data ? <FillsTable fills={fillRows} /> : <Empty>Loading fills…</Empty>}</Panel>
      : <Panel title={tab === "working" ? "Working orders" : "Order history"}>
        {orders.data ? <OrdersTable orders={visible} trading={trading} empty={tab === "working" ? "No working orders." : "No orders match."} /> : <Empty>Loading orders…</Empty>}
      </Panel>}
  </div>
}

function OrdersTable({ orders, trading, empty }: { orders: Order[]; trading: TradingStatus; empty: string }) {
  const token = useWriteToken()
  const refresh = useRefreshTrading()
  const sameSession = useTradingSession()
  const [pending, setPending] = useState<string | null>(null)
  const [error, setError] = useState<unknown>()
  const [result, setResult] = useState<string | null>(null)
  const busy = useRef(false)
  async function cancel(id: string) {
    if (busy.current || writeBlocked(trading, token)) return
    busy.current = true; setPending(id); setError(undefined)
    try {
      const response = await api.cancelOrder(id, trading.write)
      if (sameSession()) setResult(`${osiLabel(response.order.symbol, response.order.underlying)}: ${response.order.status}`)
    } catch (failure) { if (sameSession()) setError(failure) }
    finally { busy.current = false; if (sameSession()) setPending(null); void refresh() }
  }
  if (!orders.length) return <p className="text-sm text-muted">{empty}</p>
  return <div className="space-y-2">
    <TradingError error={error} />
    {result && <p className="text-xs text-muted" role="status">{result}</p>}
    <Table label="Orders" left={2} headers={["Time", "Contract", "Side", "Type", "Filled / qty", "Limit", "Avg fill", "Status", ""]}>
      {groupByDay(orders, (o) => o.accepted_at).flatMap((group) => [
        <tr key={`day-${group.day}`} className="bg-raised/40"><td colSpan={9} className="!py-1 text-[10px] uppercase tracking-wide text-muted">{group.label}</td></tr>,
        ...group.items.map((order) => <tr key={order.id}>
          <td className="text-muted">{Number.isFinite(Date.parse(order.accepted_at)) ? timeFormat.format(Date.parse(order.accepted_at)) : "—"}</td>
          <td className="!text-left"><div className="flex items-center gap-1.5 font-medium">{osiLabel(order.symbol, order.underlying)}
              {order.role && <Badge tone={order.role === "stop_loss" ? "negative" : "positive"}>{order.role === "stop_loss" ? "Stop" : "Target"}</Badge>}
              {order.bracket && <Badge tone="neutral">Bracket</Badge>}</div>
            <div className="text-[10px] text-faint">#{order.id}{order.origin === "system" ? " · system" : ""}{order.parent ? ` · for #${order.parent}` : ""}
              {order.trigger ? ` · when ${describeTrigger(order.trigger, order.side, order.underlying)}` : ""}
              {order.triggered_at ? " · triggered" : ""}</div></td>
          <td className={order.side === "buy" ? "text-bullish" : "text-bearish"}>{order.side.toUpperCase()}</td>
          <td>{order.type} · {order.time_in_force}</td>
          <td>{order.filled_quantity} / {order.quantity}</td>
          <td>{order.limit_price ? formatMoney(order.limit_price) : "MKT"}</td>
          <td>{formatMoney(order.average_fill_price)}</td>
          <td><div className="flex flex-col items-end gap-1">
            <Badge tone={statusTone[order.status]}>{statusLabel[order.status]}</Badge>
            {order.reason && <span className="max-w-64 truncate text-[10px] text-muted" title={order.reason.message}>{order.reason.code === "USER_CANCEL" ? "Cancelled by you" : order.reason.code}</span>}
          </div></td>
          <td>{open(order) && order.origin !== "system" && <button type="button" className="trade-button" aria-label={`Cancel order ${order.id} for ${order.symbol}`}
            disabled={pending != null || writeBlocked(trading, token)} onClick={() => void cancel(order.id)}>{pending === order.id ? "Cancelling…" : "Cancel"}</button>}</td>
        </tr>),
      ])}
    </Table>
  </div>
}

function FillsTable({ fills }: { fills: Fill[] }) {
  if (!fills.length) return <p className="text-sm text-muted">No fills yet.</p>
  return <Table label="Fills" left={2} headers={["Time", "Contract", "Side", "Qty", "Price", "Fee", "Order"]}>
    {groupByDay(fills, (f) => f.time).flatMap((group) => [
      <tr key={`day-${group.day}`} className="bg-raised/40"><td colSpan={7} className="!py-1 text-[10px] uppercase tracking-wide text-muted">{group.label}</td></tr>,
      ...group.items.map((fill) => <tr key={fill.id}>
        <td className="text-muted">{Number.isFinite(Date.parse(fill.time)) ? timeFormat.format(Date.parse(fill.time)) : "—"}</td>
        <td className="!text-left font-medium">{osiLabel(fill.symbol, fill.underlying)}</td>
        <td className={fill.side === "buy" ? "text-bullish" : "text-bearish"}>{fill.side.toUpperCase()}</td>
        <td>{fill.quantity}</td><td>{formatMoney(fill.price)}</td><td>{formatMoney(fill.fee)}</td><td>#{fill.order_id}</td>
      </tr>),
    ])}
  </Table>
}
