import { useQuery } from "@tanstack/react-query"
import { useEffect, useLayoutEffect, useMemo, useRef, useState } from "react"
import { api } from "../api/client"
import { useLive } from "../api/live"
import type { ChainRow, Expiry, OptionQuote } from "../api/types"
import { ExpiryPicker } from "../components/ExpiryPicker"
import { Flash } from "../components/Flash"
import { CoverageBadge, Empty, Panel, Segmented, Stat } from "../components/ui"
import { expiryCoverage } from "../lib/coverage"
import { count, days, fixed, isNum, money, pct, price, vol } from "../lib/format"
import { matchingPayload } from "../lib/payload"
import { americanApproximation, rateSourceHint } from "../lib/model"
import { OrderTicket, type TicketSelection } from "../components/OrderTicket"
import { StrategyTicket } from "../components/StrategyTicket"
import { MAX_LEGS, strategyLabel, toggleLeg, type StrategyLeg } from "../lib/strategy"
import { Dialog } from "../components/Dialog"
import { usePortfolio } from "../api/trading"
import { useMediaQuery } from "../lib/media"
import { sideFromCell } from "../lib/trading"

const windows = [
  { value: 0.02, label: "±2%" },
  { value: 0.05, label: "±5%" },
  { value: 0.1, label: "±10%" },
  { value: 0, label: "All" },
]

export function defaultExpiry(expiries: Expiry[]): string | null {
  // Skip anything expiring within the hour; it is all noise.
  return (expiries.find((e) => (e.days ?? 0) > 1 / 24) ?? expiries[0])?.id ?? null
}

/// Median distance, in vol points, between our IVs and the provider's on the
/// out-of-the-money side (the one both sides of the smile come from).
function vendorAgreement(rows: ChainRow[], forward: number | null) {
  if (!isNum(forward)) return null
  const diffs: number[] = []
  for (const row of rows) {
    const side = row.strike >= forward ? row.call : row.put
    if (side && isNum(side.iv) && isNum(side.vendor_iv)) diffs.push(Math.abs(side.iv - side.vendor_iv) * 100)
  }
  if (diffs.length === 0) return null
  diffs.sort((a, b) => a - b)
  return { median: diffs[Math.floor(diffs.length / 2)] ?? 0, count: diffs.length }
}

export function ChainView({ symbol, expiry, onExpiry }: { symbol: string; expiry: string | null; onExpiry: (id: string) => void }) {
  const live = useLive()
  const version = live.version(symbol)
  const [window, setWindow] = useState(0.05)
  const [showGreeks, setShowGreeks] = useState(false)
  const [ticket, setTicket] = useState<TicketSelection | null>(null)
  // Strategy mode collects up to four legs from the chain for one multi-leg order.
  const [mode, setMode] = useState<"single" | "strategy">("single")
  const [legs, setLegs] = useState<StrategyLeg[]>([])
  // Narrow screens collect legs in a bar and open the ticket only to review, so the chain stays usable.
  const [reviewing, setReviewing] = useState(false)
  const [untradable, setUntradable] = useState<string | null>(null)
  // Wide screens dock the ticket beside the chain so quotes stay visible.
  const docked = useMediaQuery("(min-width: 1280px)")
  const positions = usePortfolio().data?.positions
  const held = useMemo(() => new Map((positions ?? []).map((p) => [p.symbol, p.quantity])), [positions])

  const summary = useQuery({
    queryKey: ["summary", symbol, version],
    queryFn: ({ signal }) => api.summary(symbol, signal),
    placeholderData: (previous) => matchingPayload(previous, symbol),
  })
  const summaryData = matchingPayload(summary.data, symbol)
  const expiries = summaryData?.expiries ?? []
  const selected = expiry && expiries.some((e) => e.id === expiry) ? expiry : defaultExpiry(expiries)
  useEffect(() => { setTicket(null); setLegs([]); setReviewing(false); setUntradable(null) }, [symbol, selected, live.accountScope])

  const chain = useQuery({
    queryKey: ["chain", symbol, selected, window, version],
    queryFn: ({ signal }) => api.chain(symbol, selected as string, window, signal),
    enabled: selected != null,
    placeholderData: (previous) => matchingPayload(previous, symbol, selected),
  })

  const data = matchingPayload(chain.data, symbol, selected)
  const forward = data?.expiry.forward ?? null
  const atmStrike = useMemo(() => {
    if (!data || !isNum(forward)) return null
    let best: number | null = null
    for (const row of data.strikes) if (best == null || Math.abs(row.strike - forward) < Math.abs(best - forward)) best = row.strike
    return best
  }, [data, forward])
  const agreement = useMemo(() => (data ? vendorAgreement(data.strikes, forward) : null), [data, forward])
  // Legs follow the chain's latest quotes.
  const liveLegs = useMemo(() => legs.map((leg) => ({ ...leg, quote: data?.strikes.find((row) => row.strike === leg.strike)?.[leg.type] ?? leg.quote })), [legs, data])
  const highlighted = useMemo(() => new Map<string, "bid" | "ask">(mode === "strategy"
    ? legs.map((leg) => [leg.symbol, leg.side === "buy" ? "ask" : "bid"])
    : ticket ? [[ticket.symbol, ticket.cell]] : []), [mode, legs, ticket])
  const strategyOpen = mode === "strategy" && legs.length > 0
  const provider = live.status?.provider.name ?? "vendor"

  if (summary.isError) return <Empty>{String(summary.error)}</Empty>
  if (!summaryData) return <Empty>Loading {symbol}…</Empty>

  const e = data?.expiry
  const summaryExpiry = expiries.find((item) => item.id === selected)
  const coverage = expiryCoverage(e?.coverage === undefined ? summaryExpiry?.coverage : e.coverage)
  const deamericanized = e?.deamericanized ?? summaryExpiry?.deamericanized
  const approximation = americanApproximation(symbol, summaryData.american_approximation, e?.style ?? summaryExpiry?.style, deamericanized)
  return (
    <div className="flex flex-col gap-3">
      <ExpiryPicker expiries={expiries} value={selected} onChange={onExpiry} />

      {e && (
        <div className="grid grid-cols-2 gap-2 sm:grid-cols-3 lg:grid-cols-6">
          <Stat label="Forward" value={price(e.forward)} hint="Implied from put-call parity across near-the-money strikes" />
          <Stat
            label="Rate"
            value={`${pct(e.rate, 2)}${e.rate_fitted ? "" : "*"}`}
            hint={rateSourceHint(e)}
          />
          <Stat label="ATM vol" value={vol(e.atm_iv)} hint="Smile interpolated at the forward, in vol points" />
          <Stat label="To expiry" value={days(e.days)} hint={`Settles ${e.settlement === "AM" ? "on the opening print" : "at the close"} on ${e.expiry}`} />
          <Stat label="GEX" value={money(e.gex)} tone={isNum(e.gex) && e.gex < 0 ? "negative" : "positive"} hint="Dealer hedging per 1% move for this expiry (open-interest convention)" />
          <Stat
            label={`vs ${provider} IV`}
            value={agreement ? `${fixed(agreement.median, 3)} vp` : "–"}
            hint={agreement ? `Median gap between OpenPort's IVs and ${provider}'s over ${agreement.count} out-of-the-money options` : `${provider} publishes no IVs`}
          />
        </div>
      )}

      <div className={docked && (ticket || strategyOpen) ? "grid items-start gap-3 xl:grid-cols-[minmax(0,1fr)_24rem]" : ""}>
      <Panel
        title={e ? `${symbol} ${e.expiry} ${e.settlement}` : symbol}
        actions={
          <>
            <CoverageBadge coverage={coverage} />
            {live.trading && <Segmented label="Ticket mode" value={mode} onChange={(next) => { setMode(next); setTicket(null); setLegs([]); setReviewing(false) }}
              options={[{ value: "single", label: "Single" }, { value: "strategy", label: "Strategy" }]} />}
            <Segmented label="Strike window" value={window} options={windows} onChange={setWindow} />
            <Segmented
              label="Columns"
              value={showGreeks ? "greeks" : "quotes"}
              options={[
                { value: "quotes", label: "Quotes" },
                { value: "greeks", label: "Greeks" },
              ]}
              onChange={(v) => setShowGreeks(v === "greeks")}
            />
          </>
        }
      >
        {data ? (
          <ChainTable key={`${symbol}/${selected}`} rows={data.strikes} forward={forward} atmStrike={atmStrike} showGreeks={showGreeks} provider={provider}
            selected={highlighted} held={held}
            onQuote={live.trading ? (quote, row, optionType, cell) => {
              if (!quote.tradable || !quote.symbol) { setUntradable(quote.untradable_reason ?? "Contract unavailable for paper trading"); return }
              if (mode === "strategy") {
                if (legs.length >= MAX_LEGS && !legs.some((leg) => leg.symbol === quote.symbol)) { setUntradable(`A strategy has at most ${MAX_LEGS} legs.`); return }
                setLegs((current) => toggleLeg(current, { symbol: quote.symbol!, side: sideFromCell(cell), ratio: 1, type: optionType,
                  strike: row.strike, expiry: data.expiry.id, quote }))
                return
              }
              const clickedPrice = quote[cell]
              setTicket({ symbol: quote.symbol, underlying: symbol, expiry: data.expiry, strike: row.strike, optionType, cell,
                price: isNum(clickedPrice) ? String(clickedPrice) : "", spot: data.spot })
            } : undefined} />
        ) : (
          <Empty>{chain.isError ? String(chain.error) : "Loading chain…"}</Empty>
        )}
        {deamericanized && (
          <p className="mt-2 text-[11px] text-muted">
            IVs de-Americanised: early-exercise premium removed with a Leisen-Reimer tree
          </p>
        )}
        {approximation && (
          <p className="mt-2 text-[11px] text-muted">
            IVs and Greeks use a European model, accurate for the out-of-the-money side of American-style options.
          </p>
        )}
      </Panel>
      {live.trading && strategyOpen && data && (docked ? <div className="sticky top-16">
        <StrategyTicket key={live.accountScope} legs={liveLegs} onLegs={setLegs} expiry={data.expiry} underlying={symbol} spot={data.spot}
          trading={live.trading} variant="panel" onClose={() => setLegs([])} />
      </div> : reviewing ? <StrategyTicket key={live.accountScope} legs={liveLegs} onLegs={(next) => { setLegs(next); if (!next.length) setReviewing(false) }}
          expiry={data.expiry} underlying={symbol} spot={data.spot} trading={live.trading} variant="dialog" onClose={() => setReviewing(false)} />
      : <div role="region" aria-label="Strategy legs" className="fixed inset-x-3 bottom-3 z-20 flex items-center justify-between gap-3 rounded-lg border border-accent/50 bg-panel p-3 shadow-chart">
          <div className="min-w-0 text-sm">
            <div className="truncate font-medium">{symbol} {strategyLabel(liveLegs)}</div>
            <div className="text-xs text-muted">{legs.length} of {MAX_LEGS} legs · {legs.length < 2 ? "tap another bid or ask" : "tap to add or remove"}</div>
          </div>
          <div className="flex shrink-0 gap-2">
            <button type="button" className="trade-button" onClick={() => setLegs([])}>Clear</button>
            <button type="button" className="trade-button border-accent" disabled={legs.length < 2} onClick={() => setReviewing(true)}>Review</button>
          </div>
        </div>)}
      {live.trading && mode === "single" && ticket && ticket.underlying === symbol && ticket.expiry.id === selected && <div className={docked ? "sticky top-16" : ""}>
        <OrderTicket
          key={`${live.accountScope}/${ticket.symbol}/${ticket.cell}`}
          selection={ticket} trading={live.trading} variant={docked ? "panel" : "dialog"}
          quote={data?.strikes.find((row) => row.strike === ticket.strike)?.[ticket.optionType] ?? null}
          onClose={() => setTicket(null)} />
      </div>}
      </div>
      {live.trading && untradable && <Dialog title="Paper trading unavailable" onClose={() => setUntradable(null)}><p className="text-sm text-warn">{untradable}</p></Dialog>}
    </div>
  )
}

interface Column {
  key: string
  label: string
  title: string
  render: (o: OptionQuote) => React.ReactNode
  greek?: boolean
}

function columns(provider: string): Column[] {
  return [
    { key: "oi", label: "OI", title: "Open interest (contracts)", render: (o) => count(o.oi) },
    { key: "delta", label: "Δ", title: "Delta per unit of underlying", render: (o) => fixed(o.delta, 3) },
    { key: "gamma", label: "Γ", title: "Gamma per $1 of spot", render: (o) => fixed(o.gamma, 5), greek: true },
    { key: "vega", label: "Vega", title: "Per vol point", render: (o) => fixed(o.vega, 3), greek: true },
    { key: "theta", label: "Θ", title: "per calendar day, forward held fixed (as Cboe quotes it)", render: (o) => fixed(o.theta, 3), greek: true },
    {
      key: "iv",
      label: "IV",
      title: `Implied vol from the mid (hover a cell to compare with ${provider})`,
      render: (o) => <span title={[
        isNum(o.eep) && o.eep >= 0.005 ? `IV after removing a $${fixed(o.eep, 2)} early-exercise premium` : null,
        isNum(o.vendor_iv) ? `OpenPort ${vol(o.iv)} · ${provider} ${vol(o.vendor_iv)}` : null,
      ].filter(Boolean).join(" · ") || undefined}>{vol(o.iv)}</span>,
    },
    { key: "bid", label: "Bid", title: "Best bid", render: (o) => <Flash value={o.bid}>{price(o.bid)}</Flash> },
    { key: "ask", label: "Ask", title: "Best offer", render: (o) => <Flash value={o.ask}>{price(o.ask)}</Flash> },
  ]
}

function ChainTable({
  rows,
  forward,
  atmStrike,
  showGreeks,
  provider,
  onQuote,
  selected = null,
  held,
}: {
  rows: ChainRow[]
  forward: number | null
  atmStrike: number | null
  showGreeks: boolean
  provider: string
  onQuote?: (quote: OptionQuote, row: ChainRow, type: "call" | "put", cell: "bid" | "ask") => void
  /** Cells in the ticket, by canonical OSI: the side each contract trades on. */
  selected?: Map<string, "bid" | "ask"> | null
  /** Held quantity by canonical OSI, marked beside the strike. */
  held?: Map<string, number>
}) {
  const all = columns(provider).filter((c) => showGreeks || !c.greek)
  const callColumns = all
  const putColumns = [...all].reverse()
  const cell = "px-2 py-1 text-right tabular whitespace-nowrap"
  const quoteCell = (column: Column, row: ChainRow, type: "call" | "put") => {
    const quote = row[type]
    if (!quote) return "—"
    const key = column.key
    if (!onQuote || (key !== "bid" && key !== "ask")) return column.render(quote)
    const active = quote.symbol != null && selected?.get(quote.symbol) === key
    return <button type="button" aria-pressed={active} className={`rounded px-1 underline decoration-dotted underline-offset-4 hover:bg-raised hover:text-accent ${active ? "bg-accent/15 text-accent ring-1 ring-accent/50" : key === "bid" ? "text-bearish" : "text-bullish"}`}
      aria-label={`${sideFromCell(key)} ${row.strike} ${type} at ${key} ${price(quote[key])}${quote.tradable ? "" : `: ${quote.untradable_reason ?? "unavailable"}`}`}
      title={quote.tradable ? `${sideFromCell(key)} paper order` : quote.untradable_reason ?? "Unavailable"}
      onClick={() => onQuote(quote, row, type, key)}>{column.render(quote)}</button>
  }

  // Open centred on the money, and re-centre when the expiry (and so the ATM strike) changes.
  const scroller = useRef<HTMLDivElement>(null)
  const atmRow = useRef<HTMLTableRowElement>(null)
  useLayoutEffect(() => {
    const container = scroller.current
    const row = atmRow.current
    if (!container || !row) return
    container.scrollTop = row.offsetTop - container.clientHeight / 2 + row.offsetHeight / 2
  }, [atmStrike])

  return (
    <div ref={scroller} className="relative overflow-auto" style={{ maxHeight: "calc(100vh - 345px)" }}>
      <table className="w-full border-collapse text-xs">
        <thead className="sticky top-0 z-10 bg-panel">
          <tr className="text-[11px] uppercase tracking-wide text-muted">
            <th colSpan={callColumns.length} className="pb-1 text-left font-normal text-gex-positive">Calls</th>
            <th className="pb-1 font-normal" />
            <th colSpan={putColumns.length} className="pb-1 text-right font-normal text-gex-negative">Puts</th>
          </tr>
          <tr className="border-b border-border text-muted">
            {callColumns.map((c) => (
              <th key={`c-${c.key}`} title={c.title} className={`${cell} font-normal`}>{c.label}</th>
            ))}
            <th className="px-3 py-1 text-center font-normal">Strike</th>
            {putColumns.map((c) => (
              <th key={`p-${c.key}`} title={c.title} className={`${cell} font-normal`}>{c.label}</th>
            ))}
          </tr>
        </thead>
        <tbody>
          {rows.map((row) => {
            const callItm = isNum(forward) && row.strike < forward
            const putItm = isNum(forward) && row.strike > forward
            const atm = row.strike === atmStrike
            return (
              <tr
                key={row.strike}
                ref={atm ? atmRow : undefined}
                className={`border-b border-border/40 hover:bg-raised/70 ${atm ? "outline outline-1 -outline-offset-1 outline-warn/60" : ""}`}
              >
                {callColumns.map((c) => (
                  <td key={`c-${c.key}`} className={`${cell} ${callItm ? "bg-raised/50" : ""}`}>
                    {quoteCell(c, row, "call")}
                  </td>
                ))}
                <td className="px-3 py-1 text-center tabular">
                  <div className={atm ? "text-warn" : ""}>{row.strike}</div>
                  <div className="text-[10px] text-faint">{vol(row.iv, 1)}</div>
                  {(["call", "put"] as const).map((type) => {
                    const quantity = row[type]?.symbol ? held?.get(row[type]!.symbol!) : undefined
                    return quantity ? <div key={type} className={`text-[10px] font-medium ${quantity > 0 ? "text-bullish" : "text-bearish"}`} title={`You hold ${quantity} ${type}${Math.abs(quantity) === 1 ? "" : "s"}`}>
                      {type === "call" ? "C" : "P"} {quantity > 0 ? "+" : "−"}{Math.abs(quantity)}</div> : null
                  })}
                </td>
                {putColumns.map((c) => (
                  <td key={`p-${c.key}`} className={`${cell} ${putItm ? "bg-raised/50" : ""}`}>
                    {quoteCell(c, row, "put")}
                  </td>
                ))}
              </tr>
            )
          })}
        </tbody>
      </table>
    </div>
  )
}
