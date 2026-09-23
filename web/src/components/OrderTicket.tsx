import { useQuery } from "@tanstack/react-query"
import { useEffect, useRef, useState, type ReactNode } from "react"
import { api, ApiError } from "../api/client"
import { useLive } from "../api/live"
import { useAccount, usePortfolio, useRefreshTrading, useTradingSession } from "../api/trading"
import type { NewOrder, Order, Side, TradingStatus } from "../api/trading-types"
import type { Expiry, OptionQuote } from "../api/types"
import { count, days, fixed, price } from "../lib/format"
import { buyingPowerEffect, marketability, split, strategyName } from "../lib/ticket"
import { formatMoney, limitPriceText, limitPriceTick, paperNotice, sideFromCell, stepLimitPrice, ticketEstimate, validMoney } from "../lib/trading"
import { useWriteToken } from "../lib/write-token"
import { Dialog } from "./Dialog"
import { TradingError, WriteAccess, writeBlocked } from "./TradingControls"
import { Segmented } from "./ui"

export interface TicketSelection {
  symbol: string
  underlying: string
  expiry: Expiry
  strike: number
  optionType: "call" | "put"
  cell: "bid" | "ask"
  price: string
  /** Underlying spot for the naked-short buying-power estimate. */
  spot?: number | null
  /** Initial contract count, e.g. the whole position when closing. */
  quantity?: number
}

function rejected(error: unknown) {
  return error instanceof ApiError && [400, 403, 404, 409, 422].includes(error.status)
}
function retryable(error: unknown) {
  return error instanceof ApiError ? [408, 503, 504].includes(error.status)
    : error instanceof TypeError || ((error instanceof Error || error instanceof DOMException) && ["AbortError", "TimeoutError"].includes(error.name))
}

export function OrderResult({ order, error, children }: { order?: Order; error?: unknown; children?: ReactNode }) {
  const ref = useRef<HTMLDivElement>(null)
  useEffect(() => {
    if (!order && !error) return
    ref.current?.focus({ preventScroll: true })
    ref.current?.scrollIntoView({ block: "start" })
  }, [order, error])
  return <div ref={ref} role="region" tabIndex={-1} aria-label="Order result" aria-live="polite" className="space-y-2 outline-none">
    {order && <div className="rounded-md border border-border bg-raised p-3 text-sm">
      <strong className="capitalize">{order.status === "partially_filled" ? "Partial fill" : order.status}</strong>
      <div className="mt-1 tabular">{order.filled_quantity}/{order.quantity} filled · {order.remaining_quantity} remaining · Avg {formatMoney(order.average_fill_price)}</div>
      {order.reason && <p className="mt-1 text-warn">{order.reason.code}: {order.reason.message}</p>}
    </div>}
    {rejected(error) && <p className="text-sm font-medium text-danger">Rejected</p>}
    <TradingError error={error} />
    {children}
  </div>
}

const quickSizes = [1, 5, 10, 25, 50]

/** A dialog by default; `panel` docks it beside the chain. */
export function OrderTicket({ selection, quote, trading, onClose, variant = "dialog" }: {
  selection: TicketSelection; quote: OptionQuote | null; trading: TradingStatus; onClose: () => void; variant?: "dialog" | "panel"
}) {
  const title = "Paper order"
  const body = <TicketBody selection={selection} quote={quote} trading={trading} onClose={onClose} variant={variant} />
  if (variant === "dialog") return <Dialog title={title} onClose={onClose}>{body}</Dialog>
  return (
    <aside aria-label="Order ticket" className="flex max-h-[calc(100dvh-7rem)] min-w-0 flex-col overflow-hidden rounded-lg border border-border bg-panel shadow-chart">
      <header className="flex items-center justify-between gap-3 border-b border-border px-4 py-2.5">
        <h2 className="text-xs font-medium uppercase tracking-wide text-muted">Order ticket</h2>
        <button type="button" className="trade-button" onClick={onClose} aria-label="Close order ticket">Close</button>
      </header>
      <div className="min-h-0 flex-1 space-y-4 overflow-y-auto p-4">{body}</div>
    </aside>
  )
}

function TicketBody({ selection, quote, trading, onClose, variant }: {
  selection: TicketSelection; quote: OptionQuote | null; trading: TradingStatus; onClose: () => void; variant: "dialog" | "panel"
}) {
  const { accountScope, underlyings } = useLive()
  const token = useWriteToken()
  const refresh = useRefreshTrading()
  const sameSession = useTradingSession()
  const account = useAccount().data
  const portfolio = usePortfolio().data
  const [side, setSide] = useState<Side>(sideFromCell(selection.cell))
  const [type, setType] = useState<"limit" | "market">("limit")
  const [tif, setTif] = useState<"day" | "ioc">("day")
  const [quantity, setQuantity] = useState(String(selection.quantity ?? 1))
  const [limitPrice, setLimitPrice] = useState(() => limitPriceText(selection.price))
  const [fee, setFee] = useState("")
  const [pending, setPending] = useState(false)
  const [submitted, setSubmitted] = useState(false)
  const [order, setOrder] = useState<Order>()
  const [error, setError] = useState<unknown>()
  const request = useRef<NewOrder | null>(null)
  const busy = useRef(false)
  const first = useRef<HTMLDivElement>(null)
  useEffect(() => {
    if (!submitted) first.current?.querySelector<HTMLButtonElement>('[role="radio"][aria-checked="true"]')?.focus()
  }, [submitted])
  const latest = useQuery({
    queryKey: ["trading", accountScope, "ticket-order", order?.id, trading.account_version],
    queryFn: ({ signal }) => api.orders("all", signal),
    enabled: order != null && trading.enabled,
  })
  const result = latest.data?.orders.find((item) => item.id === order?.id) ?? order
  const q = Number(quantity)
  const held = portfolio?.positions.find((p) => p.symbol === selection.symbol)?.quantity ?? 0
  const marketPrice = quote?.[side === "buy" ? "ask" : "bid"]
  const estimatedPrice = type === "limit" ? limitPrice : marketPrice != null && Number.isFinite(marketPrice) ? String(marketPrice) : null
  const serverFee = trading.fee_per_contract
  const effectiveFee = serverFee ?? (fee || null)
  const estimate = ticketEstimate(quote, side, q, estimatedPrice, effectiveFee)
  const valid = /^\d+$/.test(quantity) && Number.isSafeInteger(q * 100) && q > 0 && (type === "market" || validMoney(limitPrice)) && (effectiveFee == null || validMoney(effectiveFee))
  const untradable = quote?.tradable !== true || quote.symbol !== selection.symbol
  const notice = paperNotice(selection.underlying, underlyings.find((u) => u.symbol === selection.underlying))
  const rules = account?.rules
  const closed = account?.evaluation.enabled && account.evaluation.status !== "active"
  const opening = split(side, Number.isSafeInteger(q) ? q : 0, held).opening
  const buyOnlyBlock = rules?.buy_only && side === "sell" && opening > 0
  const blocked = writeBlocked(trading, token) || trading.kill_latched || untradable || !!notice || !!closed || !!buyOnlyBlock
  const reason = quote?.untradable_reason ?? "Contract unavailable for paper trading"
  const root = selection.symbol.slice(0, 6).trim()
  const name = strategyName(side, selection.optionType, held, Number.isSafeInteger(q) && q > 0 ? q : 1)
  const fill = marketability(side, type, limitPrice, quote)
  const effect = buyingPowerEffect({ side, quantity: q, price: estimatedPrice == null ? null : Number(estimatedPrice),
    fee: Number(effectiveFee ?? 0), held, type: selection.optionType, strike: selection.strike, spot: selection.spot })
  const available = account ? Number(account.buying_power.available) : null
  const after = effect != null && available != null ? available + effect : null
  const expiryLabel = `${selection.expiry.expiry} ${selection.expiry.settlement}`

  function newOrder() {
    request.current = null
    setOrder(undefined)
    setError(undefined)
    setSubmitted(false)
  }
  async function submit() {
    if (!valid || blocked || busy.current || order || (submitted && !retryable(error))) return
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
  const setPrice = (value: number | null | undefined) => { if (value != null && Number.isFinite(value)) setLimitPrice(limitPriceText(value.toFixed(2))) }
  const stepQuantity = (delta: number) => setQuantity(String(Math.max(1, (Number.isSafeInteger(q) ? q : 1) + delta)))

  return <>
    {(result != null || error != null) && <OrderResult order={result} error={error}>
      {(rejected(error) || result?.status === "rejected") && <button type="button" className="trade-button" onClick={newOrder}>New order</button>}
      {!order && retryable(error) && <>
        <button type="button" className="trade-button" disabled={!valid || blocked || pending} onClick={() => void submit()}>Retry same order</button>
        <p className="text-xs text-muted">Retries keep the same client order ID. Check Positions before starting another ticket if the response was interrupted.</p>
      </>}
      {!order && error != null && !rejected(error) && !retryable(error) && <p className="text-xs text-muted">Check Positions and Orders to confirm the order’s state before starting another ticket.</p>}
      {result && result.status !== "rejected" && variant === "panel" && <button type="button" className="trade-button" onClick={newOrder}>Another order</button>}
    </OrderResult>}
    <div>
      <div className="flex items-baseline justify-between gap-2">
        <div className="text-base font-semibold">{selection.underlying} <span className="font-normal text-muted">{name}</span></div>
        {variant === "dialog" && <button type="button" className="text-xs text-muted hover:text-foreground" onClick={onClose}>Cancel</button>}
      </div>
      <div className="mt-1 text-sm">{selection.strike} {selection.optionType} · {expiryLabel} <span className="text-xs text-muted">({days(selection.expiry.days)})</span></div>
      <div className="mt-0.5 break-all text-[11px] tabular text-faint">{selection.symbol}</div>
      <p className="mt-1 text-[11px] text-muted">{selection.expiry.style === "american"
        ? "American · early exercise not simulated; held into expiry settles at intrinsic"
        : "European · cash settled"} · 100 multiplier</p>
      {held !== 0 && <p className="mt-1 text-xs"><span className="text-muted">You hold</span> <span className="tabular">{held > 0 ? `${held} long` : `${-held} short`}</span></p>}
    </div>
    <div className="grid grid-cols-3 gap-2 rounded-md border border-border bg-background/40 p-3 text-xs tabular">
      <button type="button" className="text-left hover:text-accent" disabled={type === "market"} onClick={() => setPrice(quote?.bid)} title="Use the bid as the limit">
        <span className="text-muted">Bid</span><div className="text-bearish">{price(quote?.bid)} × {count(quote?.bid_size)}</div></button>
      <button type="button" className="text-left hover:text-accent" disabled={type === "market"} onClick={() => setPrice(quote?.mid)} title="Use the mid as the limit">
        <span className="text-muted">Mid</span><div>{price(quote?.mid)}</div></button>
      <button type="button" className="text-left hover:text-accent" disabled={type === "market"} onClick={() => setPrice(quote?.ask)} title="Use the ask as the limit">
        <span className="text-muted">Ask</span><div className="text-bullish">{price(quote?.ask)} × {count(quote?.ask_size)}</div></button>
    </div>
    <WriteAccess trading={trading} />
    {notice && <p role="status" className="text-sm text-warn">{notice}</p>}
    {(!trading.enabled || trading.kill_latched || untradable) && <p role="status" className="text-sm text-warn">{!trading.enabled ? trading.reason ?? "Trading unavailable" : trading.kill_latched ? "Kill switch latched. Reset it in Positions before placing orders." : reason}</p>}
    {closed && <p role="status" className="text-sm text-warn">The evaluation has {account?.evaluation.status}. Start a new attempt from the Dashboard to trade again.</p>}
    {buyOnlyBlock && <p role="status" className="text-sm text-warn">{rules?.plan ?? "This plan"} is buy-only: sells may only close contracts you hold{held > 0 ? ` (${held} long)` : ""}.</p>}
    <form className="space-y-4" onSubmit={(event) => { event.preventDefault(); void submit() }}>
      <fieldset disabled={submitted || pending} className="grid min-w-0 grid-cols-2 gap-3 disabled:opacity-70">
        <legend className="sr-only">Order details</legend>
        <div ref={first} className="trade-label">Side
          <Segmented label="Side" value={side} onChange={setSide} options={[{ value: "buy", label: "Buy" }, { value: "sell", label: "Sell" }]} />
        </div>
        <div className="trade-label">Order type
          <Segmented label="Order type" value={type} onChange={(next) => { setType(next); if (next === "market") setTif("ioc") }}
            options={[{ value: "limit", label: "Limit" }, { value: "market", label: "Market" }]} />
        </div>
        <div className="trade-label col-span-2">
          <label className="trade-label">Quantity<input className="trade-input" inputMode="numeric" type="number" min="1" step="1" value={quantity} onChange={(e) => setQuantity(e.target.value)} required /></label>
          <div className="flex flex-wrap items-center gap-1.5">
            <button type="button" className="trade-button" aria-label="Decrease quantity" onClick={() => stepQuantity(-1)}>−</button>
            <button type="button" className="trade-button" aria-label="Increase quantity" onClick={() => stepQuantity(1)}>+</button>
            {quickSizes.map((size) => <button key={size} type="button" className={`trade-button ${q === size ? "border-accent" : ""}`} aria-label={`Quantity ${size}`} onClick={() => setQuantity(String(size))}>{size}</button>)}
          </div>
        </div>
        <div className="trade-label col-span-2">Time in force
          <Segmented label="Time in force" value={type === "market" ? "ioc" : tif} onChange={(next) => { if (type !== "market") setTif(next) }}
            options={[{ value: "day", label: "Day" }, { value: "ioc", label: "IOC" }]} />
        </div>
        {type === "limit" && <div className="trade-label col-span-2">
          <label className="trade-label">Limit price ($)<input className="trade-input" inputMode="decimal" value={limitPrice} onChange={(e) => setLimitPrice(e.target.value)} onBlur={() => setLimitPrice(limitPriceText(limitPrice))} onKeyDown={(e) => { if (e.key === "ArrowUp" || e.key === "ArrowDown") { e.preventDefault(); setLimitPrice(stepLimitPrice(root, limitPrice, e.key === "ArrowUp" ? 1 : -1)) } }} pattern="[0-9]+([.][0-9]+)?" required /></label>
          <div className="flex flex-wrap items-center gap-2">
            <button type="button" className="trade-button" aria-label="Decrease limit price one tick" onClick={() => setLimitPrice(stepLimitPrice(root, limitPrice, -1))}>−</button>
            <button type="button" className="trade-button" aria-label="Increase limit price one tick" onClick={() => setLimitPrice(stepLimitPrice(root, limitPrice, 1))}>+</button>
            <span className="text-xs text-muted">{formatMoney(limitPriceTick(root, limitPrice))} tick · {root}</span>
          </div>
        </div>}
        {serverFee == null ? <label className="trade-label col-span-2">Fee / contract ($, estimate)<input className="trade-input" inputMode="decimal" value={fee} placeholder="Not provided by server" onChange={(e) => setFee(e.target.value)} pattern="[0-9]+([.][0-9]+)?" /></label>
          : <div className="trade-label col-span-2">Fee / contract<div className="tabular text-foreground">{formatMoney(serverFee)}</div></div>}
      </fieldset>
      <p role="status" className={`rounded-md border px-3 py-2 text-xs ${fill.marketable ? "border-accent/40 text-foreground" : "border-border text-muted"}`}>{fill.message}</p>
      <dl className="grid grid-cols-2 gap-x-3 gap-y-2 rounded-md border border-border p-3 text-xs">
        <dt className="text-muted">Estimated premium · {side === "buy" ? "debit" : "credit"}</dt><dd className="text-right tabular">{formatMoney(estimate.premium)}</dd>
        <dt className="text-muted">Estimated fees</dt><dd className="text-right tabular">{formatMoney(estimate.fees)}</dd>
        <dt className="text-muted">Buying power effect</dt><dd className={`text-right tabular ${effect != null && effect < 0 ? "text-bearish" : ""}`}>{effect == null ? "—" : formatMoney(effect.toFixed(2))}</dd>
        {available != null && <><dt className="text-muted">Buying power after</dt><dd className={`text-right tabular ${after != null && after < 0 ? "text-danger" : ""}`}>{after == null ? "—" : formatMoney(after.toFixed(2))}</dd></>}
      </dl>
      {rules?.buying_power && after != null && after < 0 && opening > 0 && <p role="status" className="text-xs text-danger">Exceeds available buying power; the server will reject it.</p>}
      <details className="text-xs">
        <summary className="cursor-pointer text-muted">This order’s Greeks impact</summary>
        <p className="mb-2 mt-2 text-muted">Quantity × 100 × per-unit Greek, signed by side</p>
        <dl className="grid grid-cols-2 gap-2 tabular sm:grid-cols-4">{(["delta", "gamma", "vega", "theta"] as const).map((key) => <div key={key}><dt className="capitalize text-muted">{key}</dt><dd>{fixed(estimate[key], key === "gamma" ? 4 : 2)}</dd></div>)}</dl>
      </details>
      <p className="text-[11px] text-muted">Estimates use the {type === "limit" ? "limit price" : "current executable quote"}. Fills and fees are determined by the server.{type === "market" ? " Market orders always use IOC." : ""}</p>
      {(!submitted || pending) && <button className={`w-full rounded-md px-3 py-2.5 text-sm font-medium text-background disabled:cursor-not-allowed disabled:opacity-50 ${side === "buy" ? "bg-bullish" : "bg-bearish"}`}
        type="submit" aria-label="Submit order" disabled={!valid || blocked || pending}>
        {pending ? "Submitting…" : `${side === "buy" ? "Buy" : "Sell"} ${valid ? q : ""} ${name}${type === "limit" && validMoney(limitPrice) ? ` @ ${formatMoney(limitPrice)}` : type === "market" ? " at market" : ""}`}
      </button>}
    </form>
  </>
}
