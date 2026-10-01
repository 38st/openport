import type { Order } from "../api/trading-types"
import { orderLabel } from "../lib/journal"
import { orderTimeline, reasonEvidence } from "../lib/orders"
import { Dialog } from "./Dialog"
import { Badge } from "./ui"

const timeFormat = new Intl.DateTimeFormat("en-US", {
  timeZone: "America/New_York", month: "short", day: "numeric", hour: "numeric", minute: "2-digit", second: "2-digit",
})
const when = (iso: string) => Number.isFinite(Date.parse(iso)) ? `${timeFormat.format(Date.parse(iso))} ET` : iso

/**
 * One order's life: when it was accepted, triggered, changed (and each change
 * refused, with why) and when it ended, with its reason's numbers, or what a
 * working order is waiting for.
 */
export function OrderDetailDialog({ order, onClose }: { order: Order; onClose: () => void }) {
  const evidence = reasonEvidence(order.reason)
  return (
    <Dialog title={`Order #${order.id}`} onClose={onClose}>
      <div className="text-sm">
        <div className="font-medium">{orderLabel(order)}</div>
        <div className="mt-0.5 text-xs text-muted">
          {order.side ? order.side.toUpperCase() : "NET"} · {order.type} · {order.time_in_force.toUpperCase()} · filled {order.filled_quantity} of {order.quantity}
          {" · "}client ID <span className="font-mono">{order.client_order_id}</span> · {order.actor ?? "unknown"}
        </div>
      </div>
      {order.waiting && <div role="status" className="rounded border border-border bg-raised/40 px-3 py-2 text-sm">
        <div className="text-xs uppercase tracking-wide text-muted">Waiting for</div>
        <div>{order.waiting.message}</div>
      </div>}
      {order.reason && <div className="text-sm">
        <div className="text-xs uppercase tracking-wide text-muted">Reason</div>
        <div><Badge tone={order.status === "rejected" ? "negative" : "neutral"}>{order.reason.code}</Badge> {order.reason.message}</div>
        {evidence && <div className="text-xs text-muted">{evidence}</div>}
      </div>}
      <div>
        <div className="mb-1 text-xs uppercase tracking-wide text-muted">History</div>
        <ol className="space-y-1 text-sm">
          {orderTimeline(order).map((entry, index) => <li key={index} className="flex gap-3">
            <span className="w-36 shrink-0 text-xs text-muted">{entry.time ? when(entry.time) : ""}</span>
            <span className={entry.tone === "negative" ? "text-bearish" : undefined}>{entry.text}
              {entry.detail && <span className="block text-xs text-muted">{entry.detail}</span>}</span>
          </li>)}
        </ol>
      </div>
    </Dialog>
  )
}
