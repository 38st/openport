import { keepPreviousData, useQuery } from "@tanstack/react-query"
import { useLayoutEffect, useMemo, useRef, useState } from "react"
import { api } from "../api/client"
import { useLive } from "../api/live"
import type { ChainRow, Expiry, OptionQuote } from "../api/types"
import { ExpiryPicker } from "../components/ExpiryPicker"
import { Flash } from "../components/Flash"
import { Empty, Panel, Segmented, Stat } from "../components/ui"
import { count, days, fixed, isNum, money, pct, price, vol } from "../lib/format"

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

  const summary = useQuery({
    queryKey: ["summary", symbol, version],
    queryFn: () => api.summary(symbol),
    placeholderData: keepPreviousData,
  })
  const expiries = summary.data?.expiries ?? []
  const selected = expiry && expiries.some((e) => e.id === expiry) ? expiry : defaultExpiry(expiries)

  const chain = useQuery({
    queryKey: ["chain", symbol, selected, window, version],
    queryFn: () => api.chain(symbol, selected as string, window),
    enabled: selected != null,
    placeholderData: keepPreviousData,
  })

  const data = chain.data?.expiry.id === selected ? chain.data : undefined
  const forward = data?.expiry.forward ?? null
  const atmStrike = useMemo(() => {
    if (!data || !isNum(forward)) return null
    let best: number | null = null
    for (const row of data.strikes) if (best == null || Math.abs(row.strike - forward) < Math.abs(best - forward)) best = row.strike
    return best
  }, [data, forward])
  const agreement = useMemo(() => (data ? vendorAgreement(data.strikes, forward) : null), [data, forward])
  const provider = live.status?.provider.name ?? "vendor"

  if (summary.isError) return <Empty>{String(summary.error)}</Empty>
  if (!summary.data) return <Empty>Loading {symbol}…</Empty>

  const e = data?.expiry
  return (
    <div className="flex flex-col gap-3">
      <ExpiryPicker expiries={expiries} value={selected} onChange={onExpiry} />

      {e && (
        <div className="grid grid-cols-2 gap-2 sm:grid-cols-3 lg:grid-cols-6">
          <Stat label="Forward" value={price(e.forward)} hint="Implied from put-call parity across near-the-money strikes" />
          <Stat
            label="Rate"
            value={`${pct(e.rate, 2)}${e.rate_fitted ? "" : "*"}`}
            hint={e.rate_fitted ? "Fitted from this expiry's put-call parity" : "* Borrowed from the longer expiries: parity cannot pin down the rate over a few days"}
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

      <Panel
        title={e ? `${symbol} ${e.expiry} ${e.settlement}` : symbol}
        actions={
          <>
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
          <ChainTable rows={data.strikes} forward={forward} atmStrike={atmStrike} showGreeks={showGreeks} provider={provider} />
        ) : (
          <Empty>{chain.isError ? String(chain.error) : "Loading chain…"}</Empty>
        )}
      </Panel>
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
    { key: "theta", label: "Θ", title: "Per calendar day", render: (o) => fixed(o.theta, 3), greek: true },
    {
      key: "iv",
      label: "IV",
      title: `Implied vol from the mid (hover a cell to compare with ${provider})`,
      render: (o) => <span title={isNum(o.vendor_iv) ? `OpenPort ${vol(o.iv)} · ${provider} ${vol(o.vendor_iv)}` : undefined}>{vol(o.iv)}</span>,
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
}: {
  rows: ChainRow[]
  forward: number | null
  atmStrike: number | null
  showGreeks: boolean
  provider: string
}) {
  const all = columns(provider).filter((c) => showGreeks || !c.greek)
  const callColumns = all
  const putColumns = [...all].reverse()
  const cell = "px-2 py-1 text-right tabular whitespace-nowrap"

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
                    {row.call ? c.render(row.call) : "–"}
                  </td>
                ))}
                <td className="px-3 py-1 text-center tabular">
                  <div className={atm ? "text-warn" : ""}>{row.strike}</div>
                  <div className="text-[10px] text-faint">{vol(row.iv, 1)}</div>
                </td>
                {putColumns.map((c) => (
                  <td key={`p-${c.key}`} className={`${cell} ${putItm ? "bg-raised/50" : ""}`}>
                    {row.put ? c.render(row.put) : "–"}
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
