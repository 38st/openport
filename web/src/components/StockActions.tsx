import { useState } from "react"
import { api } from "../api/client"
import { useLive } from "../api/live"
import type { Position, StockHolding, TradingStatus } from "../api/trading-types"
import { describeAttribution } from "../lib/attribution"
import { fixed, isNum } from "../lib/format"
import { contractLabel } from "../lib/journal"
import { formatMoney, signedMoney } from "../lib/trading"
import { Dialog } from "./Dialog"
import { useWrite } from "./OrderActions"
import { TradingError, WriteAccess } from "./TradingControls"
import { Badge, toneOf, toneText } from "./ui"

const dollars = (value: number) => formatMoney(value.toFixed(2))

/** Shares that exercise and assignment delivered, each closable at the underlying's price. */
export function SharesTable({ stocks, onClose }: { stocks: StockHolding[]; onClose?: (stock: StockHolding) => void }) {
  return <div className="max-w-full overflow-x-auto" tabIndex={0} role="region" aria-label="Shares">
    <table className="w-full text-right text-xs tabular whitespace-nowrap">
      <thead className="text-muted"><tr>{["Stock", "Shares", "Avg price", "Price / age", "Market value", "Unrealized", "Today", ""].map((h, i) =>
        <th key={h} scope="col" className={`px-2 py-2 text-[11px] font-normal uppercase tracking-wide ${i === 0 ? "text-left" : ""}`}>{h}</th>)}</tr></thead>
      <tbody className="[&_td]:px-2 [&_td]:py-2 [&_tr]:border-t [&_tr]:border-border/40 [&_td:first-child]:text-left">
        {stocks.map((stock) => <tr key={stock.symbol}>
          <td className="font-medium">{stock.symbol}</td>
          <td><Badge tone={stock.shares > 0 ? "positive" : "negative"}>{stock.shares > 0 ? `+${stock.shares} long` : `${stock.shares} short`}</Badge></td>
          <td>{formatMoney(stock.average_price)}</td>
          <td><div>{formatMoney(stock.mark)}</div>
            <div className={`mt-1 text-[11px] ${stock.fresh ? "text-muted" : "text-warn"}`}>{stock.mark_time ? stock.fresh ? "current" : "stale" : "no price yet"}</div></td>
          <td>{formatMoney(stock.market_value)}</td>
          <td className={toneText[toneOf(stock.unrealised)]}>{signedMoney(stock.unrealised)}</td>
          <td className={toneText[toneOf(stock.attribution?.total)]} title={stock.attribution ? describeAttribution(stock.attribution) : undefined}>
            {stock.attribution ? signedMoney(stock.attribution.total.toFixed(2)) : "—"}</td>
          <td>{onClose && <button type="button" className="trade-button" aria-label={`Close ${stock.symbol} shares`} onClick={() => onClose(stock)}>Close</button>}</td>
        </tr>)}
      </tbody>
    </table>
  </div>
}

/** Close some or all shares at the underlying's price. */
export function CloseSharesDialog({ stock, trading, onClose }: { stock: StockHolding; trading: TradingStatus; onClose: () => void }) {
  const write = useWrite(trading)
  const held = Math.abs(stock.shares)
  const [shares, setShares] = useState(String(held))
  const n = Number(shares)
  const valid = Number.isSafeInteger(n) && n > 0 && n <= held
  const verb = stock.shares > 0 ? "Sell" : "Buy back"
  return <Dialog title={`Close ${stock.symbol} shares`} onClose={onClose}>
    <label className="trade-label">Shares
      <input className="trade-input" type="number" min="1" max={held} step="1" value={shares} onChange={(e) => setShares(e.target.value)} /></label>
    <p className="text-sm">{verb} {valid ? n : "…"} {stock.symbol} at the underlying's price{stock.mark ? `, now ${formatMoney(stock.mark)}` : ""}, without a fee.
      Shares trade in the regular session.</p>
    <WriteAccess trading={trading} />
    <TradingError error={write.error} />
    <button type="button" className="trade-button" disabled={!valid || write.pending || write.blocked}
      onClick={() => void write.run(() => api.closeStock(stock.symbol, n === held ? null : n, trading.write), onClose)}>
      {write.pending ? "Closing…" : `${verb} ${valid ? n : ""} shares`}</button>
  </Dialog>
}

/** Exercise long equity or ETF options into shares at the underlying's price. */
export function ExerciseDialog({ position, trading, onClose }: { position: Position; trading: TradingStatus; onClose: () => void }) {
  const write = useWrite(trading)
  const { underlyings } = useLive()
  const spot = underlyings.find((u) => u.symbol === position.underlying)?.spot
  const [contracts, setContracts] = useState(String(position.quantity))
  const n = Number(contracts)
  const valid = Number.isSafeInteger(n) && n > 0 && n <= position.quantity
  const call = position.type === "call"
  const intrinsic = isNum(spot) ? Math.max(0, call ? spot - position.strike : position.strike - spot) : null
  const mark = position.mark != null ? Number(position.mark) : null
  const timeValue = intrinsic != null && mark != null ? Math.max(0, mark - intrinsic) : null
  return <Dialog title={`Exercise ${contractLabel(position)}`} onClose={onClose}>
    <label className="trade-label">Contracts
      <input className="trade-input" type="number" min="1" max={position.quantity} step="1" value={contracts} onChange={(e) => setContracts(e.target.value)} /></label>
    <p className="text-sm">{valid ? n : "…"} contract{n === 1 ? "" : "s"} close at intrinsic value{intrinsic != null ? `, ${dollars(intrinsic)} with ${position.underlying} at ${fixed(spot, 2)}` : ""},
      and {call ? "buy" : "sell"} {valid ? n * 100 : "…"} {position.underlying} shares at that price: together, the {formatMoney(String(position.strike))} strike.</p>
    {intrinsic === 0 && <p role="status" className="text-sm text-warn">The option is out of the money, so exercising it would lose value; the server refuses it.</p>}
    {timeValue != null && timeValue > 0 && <p className="text-xs text-muted">Exercising gives up about {dollars(timeValue * (valid ? n : 1) * 100)} of time value
      against the {formatMoney(position.mark)} mark; selling the option keeps it.</p>}
    <WriteAccess trading={trading} />
    <TradingError error={write.error} />
    <button type="button" className="trade-button" disabled={!valid || write.pending || write.blocked}
      onClick={() => void write.run(() => api.exercise(position.symbol, n, trading.write), onClose)}>
      {write.pending ? "Exercising…" : `Exercise ${valid ? n : ""}`}</button>
  </Dialog>
}
