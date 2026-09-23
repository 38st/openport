import { useRef, useState } from "react"
import { api } from "../api/client"
import { useRefreshTrading, useTradingSession } from "../api/trading"
import type { ClosePositionsResponse, Order, Position, TradingStatus } from "../api/trading-types"
import { contractLabel, orderLabel } from "../lib/journal"
import { closingAction, editableFields, flattenPlan, isOpen, orderChange, orderDraft, outcome, underlyingsOf } from "../lib/orders"
import { describeTrigger } from "../lib/ticket"
import { useWriteToken } from "../lib/write-token"
import { Dialog } from "./Dialog"
import { TradingError, WriteAccess, writeBlocked } from "./TradingControls"
import { Badge } from "./ui"

/** Runs one write at a time, keeping its error and pending state for this dialog. */
function useWrite(trading: TradingStatus) {
  const token = useWriteToken()
  const refresh = useRefreshTrading()
  const sameSession = useTradingSession()
  const [pending, setPending] = useState(false)
  const [error, setError] = useState<unknown>()
  const busy = useRef(false)
  const blocked = writeBlocked(trading, token)
  async function run<T>(request: () => Promise<T>, done: (result: T) => void) {
    if (busy.current || blocked) return
    busy.current = true
    setPending(true)
    setError(undefined)
    try {
      const result = await request()
      if (sameSession()) done(result)
    } catch (failure) {
      if (sameSession()) setError(failure)
    } finally {
      busy.current = false
      if (sameSession()) setPending(false)
      void refresh()
    }
  }
  return { run, pending, error, blocked }
}

function ScopePicker({ value, options, onChange, what }: { value: string | null; options: string[]; onChange: (value: string | null) => void; what: string }) {
  if (options.length < 2) return null
  return (
    <label className="flex items-center gap-2 text-sm">
      <span className="text-muted">{what}</span>
      <select className="trade-input !w-auto !py-1" value={value ?? ""} onChange={(e) => onChange(e.target.value || null)}>
        <option value="">Every underlying</option>
        {options.map((symbol) => <option key={symbol} value={symbol}>{symbol} only</option>)}
      </select>
    </label>
  )
}

/**
 * Change a resting order in place: its size, limit price or trigger level. The
 * server checks the new terms like a new order and fills a limit that now
 * crosses the market.
 */
export function EditOrderDialog({ order, trading, onClose, onDone }: {
  order: Order; trading: TradingStatus; onClose: () => void; onDone: (order: Order) => void
}) {
  const write = useWrite(trading)
  const [draft, setDraft] = useState(() => orderDraft(order))
  const fields = editableFields(order)
  const result = orderChange(order, draft)
  const side = order.side ?? "buy"
  const priceLabel = order.legs ? "Net limit per unit (negative for a credit)" : "Limit price"
  return (
    <Dialog title={`Edit order #${order.id}`} onClose={onClose}>
      <div className="text-sm">
        <div className="font-medium">{orderLabel(order)}</div>
        <div className="mt-0.5 text-xs text-muted">
          {order.side ? order.side.toUpperCase() : "NET"} · {order.type} · filled {order.filled_quantity} of {order.quantity}
          {order.role && <> · <Badge tone={order.role === "stop_loss" ? "negative" : "positive"}>{order.role === "stop_loss" ? "Stop" : "Target"}</Badge></>}
        </div>
      </div>
      <WriteAccess trading={trading} />
      <form className="space-y-3" onSubmit={(event) => {
        event.preventDefault()
        if ("change" in result) void write.run(() => api.modifyOrder(order.id, result.change, trading.write), (response) => onDone(response.order))
      }}>
        {fields.quantity && (
          <label className="block space-y-1 text-sm">
            <span className="text-muted">Quantity{order.filled_quantity ? `, including ${order.filled_quantity} filled` : ""}</span>
            <input className="trade-input" inputMode="numeric" value={draft.quantity} aria-label="Quantity"
              onChange={(e) => setDraft({ ...draft, quantity: e.target.value })} />
          </label>
        )}
        {fields.limit && (
          <label className="block space-y-1 text-sm">
            <span className="text-muted">{priceLabel}</span>
            <input className="trade-input" inputMode="decimal" value={draft.limit_price} aria-label={priceLabel}
              onChange={(e) => setDraft({ ...draft, limit_price: e.target.value })} />
          </label>
        )}
        {fields.trigger && order.trigger && (
          <label className="block space-y-1 text-sm">
            <span className="text-muted">Trigger level: activates when {describeTrigger({ ...order.trigger, level: draft.trigger_level || order.trigger.level }, side, order.underlying)}</span>
            <input className="trade-input" inputMode="decimal" value={draft.trigger_level} aria-label="Trigger level"
              onChange={(e) => setDraft({ ...draft, trigger_level: e.target.value })} />
          </label>
        )}
        {order.role && <p className="text-xs text-muted">A bracket exit's size follows the position it protects.</p>}
        {"error" in result && <p className="text-xs text-warn" role="status">{result.error}</p>}
        <p className="text-[11px] text-muted">
          The order keeps its number and fills. The new terms are checked like a new order, and a limit that crosses the market fills at once.
        </p>
        <TradingError error={write.error} />
        <button type="submit" className="trade-button" disabled={!("change" in result) || write.pending || write.blocked}>
          {write.pending ? "Saving…" : "unchanged" in result ? "No changes" : "Save changes"}
        </button>
      </form>
    </Dialog>
  )
}

/** Cancel every working and armed order, or one underlying's. */
export function CancelAllDialog({ orders, trading, onClose, onDone }: {
  orders: readonly Order[]; trading: TradingStatus; onClose: () => void; onDone: (message: string) => void
}) {
  const write = useWrite(trading)
  const [scope, setScope] = useState<string | null>(null)
  const { cancelling, exits } = flattenPlan([], orders, scope)
  return (
    <Dialog title="Cancel orders" onClose={onClose}>
      <ScopePicker value={scope} options={underlyingsOf(orders.filter(isOpen))} onChange={setScope} what="Cancel" />
      <p className="text-sm">
        This cancels {cancelling.length} open {cancelling.length === 1 ? "order" : "orders"}{scope ? ` on ${scope}` : ""}, armed ones included.
      </p>
      {exits > 0 && <p className="text-sm text-warn">{exits === 1 ? "1 bracket exit protects" : `${exits} bracket exits protect`} a position; the position stays open without it.</p>}
      <WriteAccess trading={trading} />
      <TradingError error={write.error} />
      <button type="button" className="trade-button" disabled={!cancelling.length || write.pending || write.blocked}
        onClick={() => void write.run(() => api.cancelAllOrders(scope, trading.write),
          (response) => onDone(`${response.cancelled_orders.length} ${response.cancelled_orders.length === 1 ? "order" : "orders"} cancelled`))}>
        {write.pending ? "Cancelling…" : `Cancel ${cancelling.length} ${cancelling.length === 1 ? "order" : "orders"}`}
      </button>
    </Dialog>
  )
}

/**
 * Flatten: cancel the orders in scope, then close every position in it at market,
 * short positions first. Shows each closing order's outcome afterwards.
 */
export function FlattenDialog({ positions, orders, trading, initial = null, onClose }: {
  positions: readonly Position[]; orders: readonly Order[]; trading: TradingStatus; initial?: string | null; onClose: () => void
}) {
  const write = useWrite(trading)
  const [scope, setScope] = useState<string | null>(initial)
  const [done, setDone] = useState<ClosePositionsResponse | null>(null)
  const plan = flattenPlan(positions, orders, scope)
  const waiting = positions.filter((p) => p.awaiting_settlement && (scope == null || p.underlying === scope)).length
  return (
    <Dialog title={scope ? `Flatten ${scope}` : "Close all positions"} onClose={onClose}>
      {done ? (
        <div className="space-y-3">
          <ul className="space-y-1 text-sm">
            {done.orders.map((order) => (
              <li key={order.id} className="flex flex-wrap justify-between gap-2">
                <span>{order.side === "buy" ? "Buy" : "Sell"} {order.quantity} {orderLabel(order)}</span>
                <span className={order.status === "filled" ? "text-bullish" : "text-warn"}>{outcome(order)}</span>
              </li>
            ))}
          </ul>
          {!done.orders.length && <p className="text-sm text-muted">No position needed closing.</p>}
          <p className="text-xs text-muted">{done.cancelled_orders.length} {done.cancelled_orders.length === 1 ? "order" : "orders"} cancelled first.</p>
          <button type="button" className="trade-button" onClick={onClose}>Done</button>
        </div>
      ) : (
        <>
          <ScopePicker value={scope} options={underlyingsOf(positions.filter((p) => p.quantity !== 0))} onChange={setScope} what="Close" />
          {plan.closing.length ? (
            <ul className="space-y-1 text-sm">
              {plan.closing.map((position) => (
                <li key={position.symbol} className="flex justify-between gap-2">
                  <span>{contractLabel(position)}</span>
                  <span className={position.quantity > 0 ? "text-bearish" : "text-bullish"}>{closingAction(position)} at market</span>
                </li>
              ))}
            </ul>
          ) : <p className="text-sm text-muted">No position{scope ? ` on ${scope}` : ""} can trade now.</p>}
          <p className="text-xs text-muted">
            {plan.cancelling.length ? `${plan.cancelling.length} working ${plan.cancelling.length === 1 ? "order is" : "orders are"} cancelled first. ` : ""}
            Each position closes with a market order at the displayed quote, short ones first so a spread never leaves a naked short.
            An order the account's rules refuse is reported and the rest still close.
          </p>
          {waiting > 0 && <p className="text-xs text-muted">{waiting === 1 ? "1 expired position waits" : `${waiting} expired positions wait`} for settlement.</p>}
          <WriteAccess trading={trading} />
          <TradingError error={write.error} />
          <button type="button" className="trade-button" disabled={!plan.closing.length || write.pending || write.blocked}
            onClick={() => void write.run(() => api.closePositions(scope, trading.write), setDone)}>
            {write.pending ? "Closing…" : `Close ${plan.closing.length} ${plan.closing.length === 1 ? "position" : "positions"}`}
          </button>
        </>
      )}
    </Dialog>
  )
}
