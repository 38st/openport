import { useQuery } from "@tanstack/react-query"
import { useMemo, useState } from "react"
import { api } from "../api/client"
import { useLive } from "../api/live"
import { useOpenOrders, usePortfolio } from "../api/trading"
import type { Expiry } from "../api/types"
import { bandColor, CandleChart, levelColors } from "../charts/CandleChart"
import { candleIntervals, candleLimit, chartLevels, expectedMove, intervalNames, loadChartPrefs, saveChartPrefs, type ChartPrefs } from "../lib/candles"
import { expiryLabel, isNum, price } from "../lib/format"
import { useMediaQuery } from "../lib/media"
import { Segmented } from "./ui"

const priceText = (x: number) => x.toLocaleString("en-US", { minimumFractionDigits: 2, maximumFractionDigits: 2 })

function Key({ color, children, title }: { color: string; children: string; title?: string }) {
  return (
    <span className="inline-flex items-center gap-1.5" title={title}>
      <span className="inline-block h-0.5 w-3" style={{ background: color }} />
      {children}
    </span>
  )
}

/**
 * The underlying's candles on Trade, with the account's strikes, armed underlying
 * triggers and the selected expiry's expected move drawn over them.
 */
export function PriceChart({ symbol, spot, expiry }: { symbol: string; spot: number | null; expiry: Expiry | null }) {
  const live = useLive()
  const version = live.version(symbol)
  const [prefs, setPrefs] = useState(loadChartPrefs)
  const update = (patch: Partial<ChartPrefs>) => setPrefs((current) => {
    const next = { ...current, ...patch }
    saveChartPrefs(next)
    return next
  })
  const height = useMediaQuery("(min-width: 640px)") ? 300 : 220
  const candles = useQuery({
    queryKey: ["candles", symbol, prefs.interval, version],
    queryFn: ({ signal }) => api.candles(symbol, prefs.interval, candleLimit, signal),
    placeholderData: (previous) => previous?.symbol === symbol && previous.interval === prefs.interval ? previous : undefined,
    // Vendor history arrives between the feed's own updates.
    refetchInterval: 60_000,
    enabled: !prefs.hidden,
  })
  const positions = usePortfolio().data?.positions
  const orders = useOpenOrders().data?.orders
  const levels = useMemo(() => chartLevels(symbol, positions ?? [], orders ?? []), [symbol, positions, orders])
  const move = expectedMove(spot, expiry?.atm_iv, expiry?.days)
  const band = move != null && isNum(spot) && expiry
    ? { lo: spot - move, hi: spot + move, label: `±1σ by ${expiryLabel(expiry.id)}: ±${price(move)}` }
    : null
  const data = candles.data
  const bars = data?.symbol === symbol && data.interval === prefs.interval ? data.bars : []
  const kinds = new Set(levels.map((level) => level.kind))
  const name = `${intervalNames[prefs.interval]} candles`
  return (
    <section className="min-w-0 rounded-lg border border-border bg-panel" aria-label={`${symbol} chart`}>
      <header className={`flex flex-wrap items-center justify-between gap-2 px-3 py-2 ${prefs.hidden ? "" : "border-b border-border"}`}>
        <h2 className="text-xs font-medium uppercase tracking-wide text-muted">
          {symbol} <span className="normal-case text-faint">{name}</span>
        </h2>
        <div className="flex flex-wrap items-center gap-2">
          {!prefs.hidden && <Segmented label="Chart interval" value={prefs.interval} options={candleIntervals} onChange={(interval) => update({ interval })} />}
          <button type="button" aria-expanded={!prefs.hidden} onClick={() => update({ hidden: !prefs.hidden })}
            className="rounded-md border border-border px-2 py-1 text-xs text-muted hover:text-foreground">
            {prefs.hidden ? "Show chart" : "Hide"}
          </button>
        </div>
      </header>
      {!prefs.hidden && (
        <div className="p-3">
          {bars.length ? (
            <CandleChart key={`${symbol}/${prefs.interval}`} bars={bars} interval={prefs.interval} levels={levels} band={band}
              height={height} formatPrice={priceText} label={`${symbol} ${name}`} />
          ) : (
            <div className="flex items-center justify-center px-4 text-center text-sm text-muted" style={{ height }}>
              {candles.isError ? String(candles.error)
                : candles.isPending ? "Loading bars…"
                : "No bars yet. They build from the feed's prices as it updates, and Cboe's history fills in when it arrives."}
            </div>
          )}
          <div className="mt-2 flex flex-wrap items-center gap-x-3 gap-y-1 text-[11px] text-muted">
            {kinds.has("long") && <Key color={levelColors.long} title="Strikes you hold, net long">Long strike</Key>}
            {kinds.has("short") && <Key color={levelColors.short} title="Strikes you hold, net short">Short strike</Key>}
            {kinds.has("trigger") && <Key color={levelColors.trigger} title="Armed orders that wait for the underlying to cross a level">Trigger</Key>}
            {band && <Key color={bandColor} title={`One standard deviation by ${expiry?.expiry}: spot × ATM IV × √(days / 365)`}>Expected move</Key>}
            <span className="ml-auto text-faint">ET, market-data time · drag to pan, pinch or ⌘/Ctrl-scroll to zoom, double-click to reset</span>
          </div>
        </div>
      )}
    </section>
  )
}
