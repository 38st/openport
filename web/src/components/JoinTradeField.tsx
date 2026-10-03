import { useState } from "react"
import { useLive } from "../api/live"
import type { HeldStrategy } from "../api/trading-types"
import { legsLabel } from "../lib/journal"

export function useJoinTrade(underlying: string, strategies: HeldStrategy[] = []) {
  const { accountScope } = useLive()
  const trades = strategies.filter((trade) => trade.underlying === underlying && trade.legs.some((leg) => leg.quantity !== 0))
  const [selected, setSelected] = useState({ accountScope, underlying, id: "" })
  const group = selected.accountScope === accountScope && selected.underlying === underlying && trades.some((trade) => trade.id === selected.id)
    ? selected.id : ""
  return { group, fields: trades.length > 0 ? <label className="trade-label col-span-2">Join trade
    <select className="trade-input" value={group} onChange={(e) => setSelected({ accountScope, underlying, id: e.target.value })}>
      <option value="">None</option>
      {trades.map((trade) => <option key={trade.id} value={trade.id}>
        {legsLabel(trade.legs.map((leg) => ({ symbol: leg.symbol, side: leg.quantity > 0 ? "buy" : "sell", ratio: Math.abs(leg.quantity) })), underlying)} · #{trade.id}
      </option>)}
    </select>
  </label> : null }
}
