import { useQuery } from "@tanstack/react-query"
import { useRef, useState } from "react"
import { api, ApiError } from "../api/client"
import { useLive } from "../api/live"
import { useRefreshTrading, useTradingSession } from "../api/trading"
import type { NewOrder, Order, Side, TradingStatus } from "../api/trading-types"
import type { Expiry, OptionQuote } from "../api/types"
import { count, fixed, price } from "../lib/format"
import { formatMoney, sideFromCell, ticketEstimate, validMoney } from "../lib/trading"
import { useWriteToken } from "../lib/write-token"
import { Dialog } from "./Dialog"
import { TradingError, WriteAccess, writeBlocked } from "./TradingControls"

export interface TicketSelection {
  symbol: string
  underlying: string
  expiry: Expiry
  strike: number
  optionType: "call" | "put"
  cell: "bid" | "ask"
  price: string
}

export function OrderResult({ order, error }: { order?: Order; error?: unknown }) {
  return <div aria-live="polite" className="space-y-2">
    {order && <div className="rounded-md border border-border bg-raised p-3 text-sm">
      <strong className="capitalize">{order.status === "partially_filled" ? "Partial fill" : order.status}</strong>
      <div className="mt-1 tabular">{order.filled_quantity}/{order.quantity} filled · {order.remaining_quantity} remaining · Avg {formatMoney(order.average_fill_price)}</div>
      {order.reason && <p className="mt-1 text-warn">{order.reason.code}: {order.reason.message}</p>}
    </div>}
    {error instanceof ApiError && error.status === 422 && <p className="text-sm font-medium text-danger">Rejected</p>}
    <TradingError error={error} />
  </div>
}

export function OrderTicket({ selection, quote, trading, onClose }: {
  selection: TicketSelection; quote: OptionQuote | null; trading: TradingStatus; onClose: () => void
}) {
  const { accountScope } = useLive()
  const token = useWriteToken()
  const refresh = useRefreshTrading()
  const sameSession = useTradingSession()
  const [side, setSide] = useState<Side>(sideFromCell(selection.cell))
  const [type, setType] = useState<"limit" | "market">("limit")
  const [tif, setTif] = useState<"day" | "ioc">("day")
  const [quantity, setQuantity] = useState("1")
  const [limitPrice, setLimitPrice] = useState(selection.price)
  // No fee schedule is exposed by v1; never invent an engine fee rate.
  const [fee, setFee] = useState("")
  const [pending, setPending] = useState(false)
  const [submitted, setSubmitted] = useState(false)
  const [order, setOrder] = useState<Order>()
  const [error, setError] = useState<unknown>()
  const request = useRef<NewOrder | null>(null)
  const busy = useRef(false)
  const latest = useQuery({
    queryKey: ["trading", accountScope, "ticket-order", order?.id, trading.account_version],
    queryFn: ({ signal }) => api.orders("all", signal),
    enabled: order != null && trading.enabled,
  })
  const result = latest.data?.orders.find((item) => item.id === order?.id) ?? order
  const q = Number(quantity)
  const marketPrice = quote?.[side === "buy" ? "ask" : "bid"]
  const estimatedPrice = type === "limit" ? limitPrice : marketPrice != null && Number.isFinite(marketPrice) ? String(marketPrice) : null
  const estimate = ticketEstimate(quote, side, q, estimatedPrice, fee || null)
  const valid = /^\d+$/.test(quantity) && Number.isSafeInteger(q * 100) && q > 0 && (type === "market" || validMoney(limitPrice)) && (!fee || validMoney(fee))
  const untradable = quote?.tradable !== true || quote.symbol !== selection.symbol
  const blocked = writeBlocked(trading, token) || trading.kill_latched || untradable
  const reason = quote?.untradable_reason ?? "Contract unavailable for paper trading"

  async function submit() {
    if (!valid || blocked || busy.current || order) return
    busy.current = true
    setPending(true)
    setError(undefined)
    try {
      request.current ??= {
        client_order_id: crypto.randomUUID(), symbol: selection.symbol, side, quantity: q,
        ...(type === "market" ? { type, time_in_force: "ioc" } : { type, time_in_force: tif, limit_price: limitPrice }),
      }
      setSubmitted(true)
      const response = await api.submitOrder(request.current, trading.write)
      if (sameSession()) setOrder(response.order)
    } catch (failure) {
      if (sameSession()) setError(failure)
    } finally {
      busy.current = false
      if (sameSession()) setPending(false)
      void refresh()
    }
  }
  return <Dialog title="Paper order" onClose={onClose}>
    <div>
      <div className="text-sm font-medium">{selection.underlying} · {selection.expiry.expiry} {selection.expiry.settlement} · {selection.strike} {selection.optionType}</div>
      <div className="mt-1 break-all text-xs tabular text-muted">{selection.symbol}</div>
      <p className="mt-1 text-xs text-muted">European · cash settled · 100 multiplier</p>
    </div>
    <div className="grid grid-cols-3 gap-2 rounded-md border border-border p-3 text-xs tabular">
      <div><span className="text-muted">Bid</span><div>{price(quote?.bid)} × {count(quote?.bid_size)}</div></div>
      <div><span className="text-muted">Ask</span><div>{price(quote?.ask)} × {count(quote?.ask_size)}</div></div>
      <div><span className="text-muted">Mid</span><div>{price(quote?.mid)}</div></div>
    </div>
    <WriteAccess trading={trading} />
    {(!trading.enabled || trading.kill_latched || untradable) && <p role="status" className="text-sm text-warn">{!trading.enabled ? trading.reason ?? "Trading unavailable" : trading.kill_latched ? "Kill switch latched. Reset it in Portfolio before placing orders." : reason}</p>}
    <form className="space-y-4" onSubmit={(event) => { event.preventDefault(); void submit() }}>
      <fieldset disabled={submitted || pending} className="grid min-w-0 grid-cols-2 gap-3 disabled:opacity-70">
        <legend className="sr-only">Order details</legend>
        <label className="trade-label">Side<select autoFocus className="trade-input" value={side} onChange={(e) => setSide(e.target.value as Side)}><option value="buy">Buy</option><option value="sell">Sell</option></select></label>
        <label className="trade-label">Order type<select className="trade-input" value={type} onChange={(e) => { const next = e.target.value as "limit" | "market"; setType(next); if (next === "market") setTif("ioc") }}><option value="limit">Limit</option><option value="market">Market</option></select></label>
        <label className="trade-label">Quantity<input className="trade-input" inputMode="numeric" type="number" min="1" step="1" value={quantity} onChange={(e) => setQuantity(e.target.value)} required /></label>
        <label className="trade-label">Time in force<select className="trade-input" disabled={type === "market"} value={type === "market" ? "ioc" : tif} onChange={(e) => setTif(e.target.value as "day" | "ioc")}><option value="day">Day</option><option value="ioc">IOC</option></select></label>
        {type === "limit" && <label className="trade-label">Limit price ($)<input className="trade-input" inputMode="decimal" value={limitPrice} onChange={(e) => setLimitPrice(e.target.value)} pattern="[0-9]+([.][0-9]+)?" required /></label>}
        <label className="trade-label">Fee / contract ($, estimate)<input className="trade-input" inputMode="decimal" value={fee} placeholder="Not provided by server" onChange={(e) => setFee(e.target.value)} pattern="[0-9]+([.][0-9]+)?" /></label>
      </fieldset>
      <div className="grid grid-cols-2 gap-3 text-xs">
        <div><span className="text-muted">Estimated premium · {side === "buy" ? "debit" : "credit"}</span><div className="mt-1 tabular">{formatMoney(estimate.premium)}</div></div>
        <div><span className="text-muted">Estimated fees</span><div className="mt-1 tabular">{formatMoney(estimate.fees)}</div></div>
      </div>
      <div className="text-xs">
        <p className="mb-2 text-muted">This order’s Greeks impact · quantity × 100 × per-unit Greek, signed by side</p>
        <dl className="grid grid-cols-2 gap-2 tabular sm:grid-cols-4">{(["delta", "gamma", "vega", "theta"] as const).map((key) => <div key={key}><dt className="capitalize text-muted">{key}</dt><dd>{fixed(estimate[key], key === "gamma" ? 4 : 2)}</dd></div>)}</dl>
      </div>
      <p className="text-xs text-muted">Estimates use the {type === "limit" ? "limit price" : "current executable quote"}. Fills and fees are determined by the server.{type === "market" ? " Market orders always use IOC." : ""}</p>
      <button className="trade-button" type="submit" disabled={!valid || blocked || pending || !!order || (error instanceof ApiError && [409, 422].includes(error.status))}>{pending ? "Submitting…" : submitted && !order ? "Retry same order" : "Submit paper order"}</button>
      {submitted && !order && !pending && <p className="text-xs text-muted">Retries keep the same client order ID. Check Portfolio before starting another ticket if the response was interrupted.</p>}
    </form>
    <OrderResult order={result} error={error} />
  </Dialog>
}
