import { useQuery } from "@tanstack/react-query"
import { useRef, useState } from "react"
import { api } from "../api/client"
import { useLive } from "../api/live"
import { useAccount, usePortfolio, useRefreshTrading, useTradingSession } from "../api/trading"
import type { NewOrder, Order, TradingStatus } from "../api/trading-types"
import type { Expiry } from "../api/types"
import { LineChart } from "../charts/LineChart"
import { expectedMove } from "../lib/candles"
import { isNum, money, price } from "../lib/format"
import { heldPositions } from "../lib/margin"
import { probabilityOfProfit, smileVol, valueToday } from "../lib/probability"
import { estimatedProfile, MAX_LEGS, MAX_RATIO, netQuote, riskProfile, roundNet, strategyLabel, strategyPayoff, strategyPowerUse, type StrategyLeg } from "../lib/strategy"
import { comboTickCents, formatMoney, paperNotice } from "../lib/trading"
import { useWriteToken } from "../lib/write-token"
import { Dialog } from "./Dialog"
import { OrderResult } from "./OrderTicket"
import { WriteAccess, writeBlocked } from "./TradingControls"
import { Segmented } from "./ui"

const quickSizes = [1, 2, 5, 10]
/** "$1.20 debit", "$0.80 credit", "even". */
export function netText(net: number | null | undefined): string {
  if (net == null || !Number.isFinite(net)) return "—"
  if (Math.abs(net) < 0.005) return "even"
  return `$${Math.abs(net).toFixed(2)} ${net > 0 ? "debit" : "credit"}`
}

const shortDate = (date: string) => new Date(`${date}T12:00:00Z`).toLocaleDateString("en-US", { month: "short", day: "numeric", timeZone: "UTC" })

/**
 * Builds and submits a multi-leg order from legs picked on the chain. Prices are
 * net per unit: a debit pays, a credit receives. Risk is shown at the first
 * expiry: exact when every leg expires then, estimated for calendars and diagonals.
 * `expiries` are the legs' expiries from the chain; `units` sets the starting quantity.
 * `closing` marks an order that closes held positions, and `roll` one that closes
 * them and opens the same strategy at a later expiry: either payoff alone means
 * nothing, so the ticket says what the order does instead. The `bare` variant is
 * the body alone, for a dialog that adds its own controls.
 */
export function StrategyTicket({ legs, onLegs, expiries, underlying, spot, trading, onClose, variant = "panel", units, title = "Strategy order", closing = false, roll = false, smiles }: {
  legs: StrategyLeg[]; onLegs: (legs: StrategyLeg[]) => void; expiries: Expiry[]; underlying: string
  spot: number | null | undefined; trading: TradingStatus; onClose: () => void; variant?: "dialog" | "panel" | "bare"; units?: number; title?: string
  closing?: boolean; roll?: boolean
  /** Each expiry's smile, by expiry id, for the probability of profit; flat at-the-money volatility otherwise. */
  smiles?: ReadonlyMap<string, readonly { strike: number; iv: number | null }[]>
}) {
  const body = <StrategyBody legs={legs} onLegs={onLegs} expiries={expiries} underlying={underlying} spot={spot} trading={trading} initialUnits={units} closing={closing} roll={roll} smiles={smiles} />
  if (variant === "bare") return body
  if (variant === "dialog") return <Dialog title={title} onClose={onClose}>{body}</Dialog>
  return (
    <aside aria-label="Strategy ticket" className="flex max-h-[calc(100dvh-7rem)] min-w-0 flex-col overflow-hidden rounded-lg border border-border bg-panel shadow-chart">
      <header className="flex items-center justify-between gap-3 border-b border-border px-4 py-2.5">
        <h2 className="text-xs font-medium uppercase tracking-wide text-muted">Strategy ticket</h2>
        <button type="button" className="trade-button" onClick={onClose} aria-label="Close strategy ticket">Close</button>
      </header>
      <div className="min-h-0 flex-1 space-y-4 overflow-y-auto p-4">{body}</div>
    </aside>
  )
}

function StrategyBody({ legs, onLegs, expiries, underlying, spot, trading, initialUnits, closing, roll, smiles }: {
  legs: StrategyLeg[]; onLegs: (legs: StrategyLeg[]) => void; expiries: Expiry[]; underlying: string
  spot: number | null | undefined; trading: TradingStatus; initialUnits?: number; closing: boolean; roll: boolean
  smiles?: ReadonlyMap<string, readonly { strike: number; iv: number | null }[]>
}) {
  const { accountScope, underlyings } = useLive()
  const token = useWriteToken()
  const refresh = useRefreshTrading()
  const sameSession = useTradingSession()
  const account = useAccount().data
  const positions = usePortfolio().data?.positions
  const roots = legs.map((l) => l.symbol.slice(0, 6).trim())
  const tick = comboTickCents(roots)
  const quote = netQuote(legs)
  const [units, setUnits] = useState(String(initialUnits ?? 1))
  const [type, setType] = useState<"limit" | "market">("limit")
  const [tif, setTif] = useState<"day" | "ioc">("day")
  const [amount, setAmount] = useState(() => quote.mid != null ? Math.abs(roundNet(quote.mid, tick)).toFixed(2) : "")
  const [direction, setDirection] = useState<"debit" | "credit">(() => (quote.mid ?? 0) < 0 ? "credit" : "debit")
  const [pending, setPending] = useState(false)
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
  // A different set of legs is a new strategy: reprice it at its mid and start a new order.
  const signature = legs.map((l) => `${l.symbol}:${l.side}:${l.ratio}`).join("|")
  const [pricedFor, setPricedFor] = useState(signature)
  if (pricedFor !== signature) {
    setPricedFor(signature)
    if (quote.mid != null) {
      const rounded = roundNet(quote.mid, tick)
      setAmount(Math.abs(rounded).toFixed(2))
      setDirection(rounded < 0 ? "credit" : "debit")
    }
    request.current = null
    setOrder(undefined)
    setError(undefined)
  }

  const q = Number(units)
  const validUnits = /^\d+$/.test(units) && Number.isSafeInteger(q) && q > 0
  const typed = Number(amount)
  const validAmount = /^\d+(\.\d+)?$/.test(amount) && Number.isFinite(typed) && Math.round(typed * 100) % tick === 0
  const net = type === "market" ? quote.ask : validAmount ? (direction === "debit" ? typed : -typed) : null
  const limitText = net == null ? "" : net.toFixed(2)
  const label = strategyLabel(legs)
  const legExpiries = [...new Set(legs.map((l) => l.expiry))]
  const multi = legExpiries.length > 1
  const known = expiries.filter((e) => legExpiries.includes(e.id)).sort((a, b) => Date.parse(a.expiry_time) - Date.parse(b.expiry_time))
  const terms = new Map(known.map((e) => [e.id, { id: e.id, time: Date.parse(e.expiry_time), forward: e.forward, discount: e.discount }]))
  const value = net != null && validUnits ? strategyPayoff(legs, q, net, terms) : null
  const strikes = legs.map((l) => l.strike)
  const center = spot != null && Number.isFinite(spot) ? spot : (Math.min(...strikes) + Math.max(...strikes)) / 2
  const profile = net == null || !validUnits || closing || roll ? null
    : !multi ? riskProfile(legs, q, net)
    : value ? estimatedProfile(value, legs, q, Math.max(0, Math.min(center, ...strikes) * 0.7), Math.max(center, ...strikes) * 1.3) : null
  const approx = profile?.estimated ? "≈ " : ""
  // The first expiry's risk-neutral distribution: its forward, time left and smile.
  const front = known[0]
  const distribution = front && isNum(front.forward) && isNum(front.days) && front.days > 0
    ? { forward: front.forward, years: front.days / 365, vol: smileVol(smiles?.get(front.id) ?? [], isNum(front.atm_iv) ? front.atm_iv : null) } : null
  const pop = profile && value && distribution ? probabilityOfProfit(value, profile.breakevens, distribution) : null
  const move = front ? expectedMove(front.forward, front.atm_iv, front.days) : null
  const legTerms = new Map(known.map((e) => [e.id, { forward: e.forward, discount: e.discount, years: isNum(e.days) ? e.days / 365 : null }]))
  const today = profile && net != null && validUnits && spot != null && Number.isFinite(spot) ? valueToday(legs, q, net, spot, legTerms) : null
  const fee = Number(trading.fee_per_contract ?? 0)
  const contracts = validUnits ? q * legs.reduce((total, leg) => total + leg.ratio, 0) : 0
  // Mirrors the server: margin on the held positions after the fill, plus the net and fees.
  const power = strategyPowerUse(legs, q, net, fee, spot, heldPositions(positions ?? []))
  const effect = power?.effect ?? null
  const available = account ? Number(account.buying_power.available) : null
  const after = effect != null && available != null ? available + effect : null
  const marketable = quote.ask != null && (type === "market" || (net != null && quote.ask <= net + 1e-9))
  const notice = paperNotice(underlying, underlyings.find((u) => u.symbol === underlying))
  const untradable = legs.find((l) => l.quote?.tradable !== true)
  const rules = account?.rules
  const closed = account?.evaluation.enabled && account.evaluation.status !== "active"
  const blocked = writeBlocked(trading, token) || trading.kill_latched || !!untradable || !!notice || !!closed || !!rules?.buy_only
  const valid = legs.length >= 2 && validUnits && (type === "market" || validAmount)

  // Frame the strikes and spot with a margin of the strike range or 1% of spot, whichever is
  // wider, and the expected move within a quarter of spot.
  const margin = Math.max(Math.max(...strikes) - Math.min(...strikes), center * 0.01)
  let [chartLow, chartHigh] = [Math.max(0, Math.min(center, ...strikes) - margin), Math.max(center, ...strikes) + margin]
  if (move != null && front && isNum(front.forward)) {
    chartLow = Math.max(0, Math.min(chartLow, Math.max(center * 0.75, front.forward - move * 1.15)))
    chartHigh = Math.max(chartHigh, Math.min(center * 1.25, front.forward + move * 1.15))
  }
  const grid = Array.from({ length: 121 }, (_, i) => chartLow + ((chartHigh - chartLow) * i) / 120)
  const chart = profile && value ? grid.map((x) => ({ x, y: value(x) })) : null
  const todayChart = chart && today ? grid.map((x) => ({ x, y: today(x) })) : null

  const setLeg = (index: number, patch: Partial<StrategyLeg>) => onLegs(legs.map((l, i) => (i === index ? { ...l, ...patch } : l)))
  const applyNet = (value: number | null) => {
    if (value == null) return
    const rounded = roundNet(value, tick)
    setAmount(Math.abs(rounded).toFixed(2))
    setDirection(rounded < 0 ? "credit" : "debit")
  }
  const step = (delta: number) => {
    const current = net ?? 0
    applyNet(current + (delta * tick) / 100)
  }
  function newOrder() {
    request.current = null
    setOrder(undefined)
    setError(undefined)
  }
  async function submit() {
    if (!valid || blocked || busy.current || order) return
    busy.current = true
    setPending(true)
    setError(undefined)
    try {
      request.current ??= {
        client_order_id: crypto.randomUUID(), legs: legs.map(({ symbol, side, ratio }) => ({ symbol, side, ratio })), quantity: q,
        ...(type === "market" ? { type, time_in_force: "ioc" } : { type, time_in_force: tif, limit_price: limitText }),
      }
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

  return <>
    {(result != null || error != null) && <OrderResult order={result} error={error}>
      <button type="button" className="trade-button" onClick={newOrder}>Another order</button>
    </OrderResult>}
    <div>
      <div className="text-base font-semibold">{underlying} <span className="font-normal text-muted">{closing ? "Close" : roll ? "Roll" : label}</span></div>
      <div className="mt-1 text-sm">{known.length === 1 ? `${known[0]!.expiry} ${known[0]!.settlement}` : known.map((e) => shortDate(e.expiry)).join(" / ")} · {legs.length} of {MAX_LEGS} legs</div>
      {legs.length < 2 && <p className="mt-1 text-xs text-muted">Click another bid or ask on the chain to add a leg: an ask buys, a bid sells.</p>}
    </div>
    <div className="overflow-hidden rounded-md border border-border">
      <table className="w-full text-xs tabular">
        <thead className="bg-raised/40 text-[10px] uppercase tracking-wide text-muted"><tr>
          <th className="px-2 py-1.5 text-left font-normal">Side</th><th className="px-2 py-1.5 text-left font-normal">Ratio</th>
          <th className="px-2 py-1.5 text-left font-normal">Contract</th><th className="px-2 py-1.5 text-right font-normal">Bid × ask</th><th />
        </tr></thead>
        <tbody>{legs.map((leg, i) => (
          <tr key={leg.symbol} className="border-t border-border/40">
            <td className="px-2 py-1.5"><button type="button" className={`rounded px-1.5 py-0.5 text-[11px] font-medium ${leg.side === "buy" ? "bg-bullish/15 text-bullish" : "bg-bearish/15 text-bearish"}`}
              aria-label={`${leg.side === "buy" ? "Buy" : "Sell"} ${leg.strike} ${leg.type}: switch side`} onClick={() => setLeg(i, { side: leg.side === "buy" ? "sell" : "buy" })}>
              {leg.side === "buy" ? "BUY" : "SELL"}</button></td>
            <td className="px-2 py-1.5"><span className="inline-flex items-center gap-1">
              <button type="button" className="px-1 text-muted hover:text-foreground" aria-label={`Fewer ${leg.strike} ${leg.type} per unit`} disabled={leg.ratio <= 1} onClick={() => setLeg(i, { ratio: leg.ratio - 1 })}>−</button>
              {leg.ratio}
              <button type="button" className="px-1 text-muted hover:text-foreground" aria-label={`More ${leg.strike} ${leg.type} per unit`} disabled={leg.ratio >= MAX_RATIO} onClick={() => setLeg(i, { ratio: leg.ratio + 1 })}>+</button>
            </span></td>
            <td className="px-2 py-1.5">{leg.strike} {leg.type === "call" ? "C" : "P"}{multi && <span className="text-muted"> · {shortDate(leg.expiry.slice(0, 10))}</span>}</td>
            <td className="px-2 py-1.5 text-right">{price(leg.quote?.bid)} × {price(leg.quote?.ask)}</td>
            <td className="px-1 py-1.5 text-right"><button type="button" className="px-1 text-muted hover:text-danger" aria-label={`Remove ${leg.strike} ${leg.type}`}
              onClick={() => onLegs(legs.filter((_, j) => j !== i))}>×</button></td>
          </tr>
        ))}</tbody>
      </table>
    </div>
    {legs.length >= 2 && <>
      <div className="grid grid-cols-3 gap-2 rounded-md border border-border bg-background/40 p-3 text-xs tabular">
        {([["Net bid", quote.bid], ["Net mid", quote.mid], ["Net ask", quote.ask]] as const).map(([name, value]) => (
          <button key={name} type="button" className="text-left hover:text-accent" disabled={type === "market" || value == null} onClick={() => applyNet(value)} title={`Use the ${name.toLowerCase()} as the limit`}>
            <span className="text-muted">{name}</span><div>{netText(value)}</div></button>
        ))}
      </div>
      <WriteAccess trading={trading} />
      {notice && <p role="status" className="text-sm text-warn">{notice}</p>}
      {untradable && <p role="status" className="text-sm text-warn">{untradable.strike} {untradable.type}: {untradable.quote?.untradable_reason ?? "unavailable for paper trading"}</p>}
      {trading.kill_latched && <p role="status" className="text-sm text-warn">Kill switch latched. Reset it in Positions before placing orders.</p>}
      {closed && <p role="status" className="text-sm text-warn">The evaluation has {account?.evaluation.status}. Start a new attempt from the Dashboard to trade again.</p>}
      {rules?.buy_only && <p role="status" className="text-sm text-warn">{rules.plan ?? "This plan"} is buy-only and single-leg. Strategies need a plan that allows any strategy.</p>}
      <form className="space-y-4" onSubmit={(event) => { event.preventDefault(); void submit() }}>
        <fieldset disabled={pending || order != null} className="grid min-w-0 grid-cols-2 gap-3 disabled:opacity-70">
          <legend className="sr-only">Strategy order</legend>
          <div className="trade-label col-span-2">
            <label className="trade-label">Quantity (units)<input className="trade-input" inputMode="numeric" type="number" min="1" step="1" value={units} onChange={(e) => setUnits(e.target.value)} required /></label>
            <div className="flex flex-wrap items-center gap-1.5">
              {quickSizes.map((size) => <button key={size} type="button" className={`trade-button ${q === size ? "border-accent" : ""}`} aria-label={`Quantity ${size}`} onClick={() => setUnits(String(size))}>{size}</button>)}
              {validUnits && <span className="text-xs text-muted">{contracts} contracts</span>}
            </div>
          </div>
          <div className="trade-label">Order type
            <Segmented label="Order type" value={type} onChange={(next) => { setType(next); if (next === "market") setTif("ioc") }}
              options={[{ value: "limit", label: "Limit" }, { value: "market", label: "Market" }]} />
          </div>
          <div className="trade-label">Time in force
            <Segmented label="Time in force" value={type === "market" ? "ioc" : tif} onChange={(next) => { if (type !== "market") setTif(next) }}
              options={[{ value: "day", label: "Day" }, { value: "ioc", label: "IOC" }]} />
          </div>
          {type === "limit" && <div className="trade-label col-span-2">
            <span className="flex flex-wrap items-end gap-2">
              <label className="trade-label min-w-0 flex-1">Net limit ($)<input className="trade-input" inputMode="decimal" value={amount} onChange={(e) => setAmount(e.target.value)} pattern="[0-9]+([.][0-9]+)?" required /></label>
              <Segmented label="Net direction" value={direction} onChange={setDirection} options={[{ value: "debit", label: "Debit" }, { value: "credit", label: "Credit" }]} />
            </span>
            <span className="flex flex-wrap items-center gap-2">
              <button type="button" className="trade-button" aria-label="Lower the net one tick" onClick={() => step(-1)}>−</button>
              <button type="button" className="trade-button" aria-label="Raise the net one tick" onClick={() => step(1)}>+</button>
              <span className={`text-xs ${amount && !validAmount ? "text-warn" : "text-muted"}`}>{amount && !validAmount ? `Use a multiple of $${(tick / 100).toFixed(2)}` : `$${(tick / 100).toFixed(2)} tick`} · {direction === "debit" ? "pay at most" : "receive at least"}</span>
            </span>
          </div>}
        </fieldset>
        <p role="status" className={`rounded-md border px-3 py-2 text-xs ${marketable ? "border-accent/40 text-foreground" : "border-border text-muted"}`}>
          {quote.ask == null ? "Every leg needs a two-sided quote before the strategy can fill."
            : marketable ? `Marketable: fills now at ${netText(quote.ask)} per unit, all legs together, up to each leg's displayed size.`
            : `Rests: the legs trade now at ${netText(quote.ask)}; fills when that reaches ${netText(net)}.`}
        </p>
        <dl className="grid grid-cols-2 gap-x-3 gap-y-2 rounded-md border border-border p-3 text-xs">
          <dt className="text-muted">Net premium</dt><dd className="text-right tabular">{net == null || !validUnits ? "—" : `${formatMoney((Math.abs(net) * 100 * q).toFixed(2))} ${net > 0 ? "paid" : "received"}`}</dd>
          <dt className="text-muted">Estimated fees</dt><dd className="text-right tabular">{validUnits ? formatMoney((fee * contracts).toFixed(2)) : "—"}</dd>
          {closing ? <><dt className="text-muted">Closes</dt><dd className="text-right tabular">{validUnits ? `${contracts} contracts, all legs together` : "—"}</dd></>
          : roll ? <><dt className="text-muted">Rolls</dt><dd className="text-right tabular">{validUnits && known.length > 1 ? `${q} unit${q === 1 ? "" : "s"}, ${shortDate(known[0]!.expiry)} to ${shortDate(known[known.length - 1]!.expiry)}` : "—"}</dd></> : <>
          <dt className="text-muted">Max profit</dt><dd className="text-right tabular text-bullish">{profile ? profile.maxProfit == null ? "Unlimited" : `${approx}${formatMoney(profile.maxProfit.toFixed(2))}` : "—"}</dd>
          <dt className="text-muted">Max loss</dt><dd className="text-right tabular text-bearish">{profile ? profile.maxLoss == null ? "Unlimited" : `${approx}${formatMoney(profile.maxLoss.toFixed(2))}` : "—"}</dd>
          <dt className="text-muted">Breakevens</dt><dd className="text-right tabular">{profile ? profile.breakevens.length ? `${approx}${profile.breakevens.map((b) => b.toFixed(2)).join(", ")}` : "None" : "—"}</dd>
          <dt className="text-muted" title="Risk-neutral: lognormal around the expiry's forward, with the smile's volatility at each breakeven">Probability of profit</dt>
          <dd className="text-right tabular">{pop == null ? "—" : `≈ ${(pop * 100).toFixed(0)}%`}</dd>
          <dt className="text-muted" title="One standard deviation: forward × ATM volatility × √(years)">1σ move{front ? ` by ${shortDate(front.expiry)}` : ""}</dt>
          <dd className="text-right tabular">{move == null ? "—" : `±${move.toFixed(2)}`}</dd>
          </>}
          <dt className="text-muted">Buying power effect</dt><dd className={`text-right tabular ${effect != null && effect < 0 ? "text-bearish" : ""}`}>{effect == null ? "—" : formatMoney(effect.toFixed(2))}</dd>
          {available != null && <><dt className="text-muted">Buying power after</dt><dd className={`text-right tabular ${after != null && after < 0 ? "text-danger" : ""}`}>{after == null ? "—" : formatMoney(after.toFixed(2))}</dd></>}
        </dl>
        {rules?.buying_power && after != null && after < 0 && power?.uses && <p role="status" className="text-xs text-danger">Exceeds available buying power; the server will reject it.</p>}
        {chart && <figure aria-label="Profit and loss at expiry">
          <LineChart height={160} marginLeft={60} series={[{ id: "payoff", label: "P&L at expiry", color: "var(--chart-1)", points: chart, area: true },
            ...(todayChart ? [{ id: "today", label: "P&L today", color: "var(--chart-4)", points: todayChart, dashed: true }] : [])]}
            references={[{ y: 0, label: "Even", color: "var(--muted)" }]} markers={[
              ...(spot != null && Number.isFinite(spot) ? [{ x: spot, label: `${underlying} ${spot.toFixed(0)}`, color: "var(--warn)" }] : []),
              ...(move != null && front && isNum(front.forward) ? [{ x: front.forward - move, label: "−1σ", color: "var(--chart-7)" }, { x: front.forward + move, label: "+1σ", color: "var(--chart-7)" }] : []),
            ]}
            formatX={(x) => x.toFixed(0)} formatY={(y) => money(y)} />
          <figcaption className="mt-1 text-[11px] text-muted">{profile?.estimated
            ? `Estimated P&L at the ${shortDate(known[0]!.expiry)} expiry for ${q} unit${q === 1 ? "" : "s"}, later legs at today's implied volatility, before fees.`
            : `P&L at expiry for ${validUnits ? q : "—"} unit${q === 1 ? "" : "s"}, before fees.`}{todayChart ? " Dashed: today, at each leg's implied volatility." : ""}</figcaption>
        </figure>}
        <button className="w-full rounded-md bg-accent px-3 py-2.5 text-sm font-medium text-background disabled:cursor-not-allowed disabled:opacity-50"
          type="submit" aria-label="Submit strategy order" disabled={!valid || blocked || pending || order != null}>
          {pending ? "Submitting…" : `${label} · ${validUnits ? q : ""} × ${type === "market" ? "at market" : netText(net)}`}
        </button>
      </form>
    </>}
  </>
}
