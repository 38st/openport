import { useState } from "react"
import type { Bracket, Trigger } from "../api/trading-types"
import { roundNet } from "../lib/strategy"
const netText = (net: number) => `$${Math.abs(net).toFixed(2)} ${net < 0 ? "credit" : "debit"}`

/** Closing net prices use the same signed debit convention as combo orders. */
export function spreadTarget(entry: number, percent: number, tick: number): string {
  return roundNet(-entry * percent / 100, tick).toFixed(2)
}

export function useSpreadExits(entry: number | null, tick: number, enabled = false) {
  const [protect, setProtect] = useState(enabled)
  const [targetOn, setTargetOn] = useState(true)
  const [stopOn, setStopOn] = useState(true)
  const [percent, setPercent] = useState(entry != null && entry > 0 ? "150" : "50")
  const [source, setSource] = useState<"combo" | "underlying">("combo")
  const [direction, setDirection] = useState<Trigger["direction"]>("at_or_above")
  const [level, setLevel] = useState("")
  const targetValid = entry != null && Number.isFinite(entry) && /^\d+(\.\d+)?$/.test(percent) && Number(percent) > 0
  const stopValid = /^-?\d+(\.\d+)?$/.test(level) && Number.isFinite(Number(level)) && (source === "combo" || Number(level) > 0)
  const valid = !protect || ((targetOn || stopOn) && (!targetOn || targetValid) && (!stopOn || stopValid))
  const target = targetValid ? spreadTarget(entry!, Number(percent), tick) : null
  const bracket: Bracket | undefined = protect && valid ? {
    ...(targetOn && target != null ? { take_profit: { limit_price: target } } : {}),
    ...(stopOn ? { stop_loss: { trigger: { source, direction, level } } } : {}),
  } : undefined
  const fields = <div className="space-y-3 text-xs">
    <label className="flex items-center gap-2"><input type="checkbox" checked={protect} onChange={(e) => setProtect(e.target.checked)} />Spread exits</label>
    {protect && <>
      <label className="flex items-center gap-2"><input type="checkbox" checked={targetOn} onChange={(e) => setTargetOn(e.target.checked)} />Take profit</label>
      {targetOn && <label className="trade-label">{entry != null && entry > 0 ? "Receive (% of entry debit)" : "Buy back (% of entry credit)"}
        <input className="trade-input" inputMode="decimal" value={percent} onChange={(e) => setPercent(e.target.value)} />
        <span className="text-muted">Closing limit: {target == null ? "—" : netText(Number(target))}</span>
      </label>}
      <label className="flex items-center gap-2"><input type="checkbox" checked={stopOn} onChange={(e) => setStopOn(e.target.checked)} />Stop loss</label>
      {stopOn && <>
        <label className="trade-label">Stop source<select className="trade-input" value={source} onChange={(e) => setSource(e.target.value as "combo" | "underlying")}>
          <option value="combo">Closing combo net</option><option value="underlying">Underlying</option>
        </select></label>
        <label className="trade-label">Stop direction<select className="trade-input" value={direction} onChange={(e) => setDirection(e.target.value as Trigger["direction"])}>
          <option value="at_or_above">At or above</option><option value="at_or_below">At or below</option>
        </select></label>
        <label className="trade-label">Stop level<input className="trade-input" inputMode="decimal" value={level} onChange={(e) => setLevel(e.target.value)} /></label>
        <p className="text-muted">Combo net is the displayed cost to close: positive to pay, negative to receive. A reached stop sends a market IOC combo with the plan’s slippage.</p>
      </>}
      <p className="text-muted">Exits close all legs together, wait outside the regular session, and expire at the nearest leg’s last trade or auto-close. A fill cancels the other exit.</p>
    </>}
  </div>
  return { bracket, valid, fields }
}
