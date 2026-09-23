import { useLive } from "../api/live"
import { Panel, FeedBadge } from "../components/ui"
import { AsOf } from "../components/AsOf"
import { clock, count, fixed, price } from "../lib/format"

function Row({ label, children }: { label: string; children: React.ReactNode }) {
  return (
    <div className="flex items-baseline justify-between gap-4 border-b border-border/40 py-1.5 text-sm last:border-0">
      <span className="text-muted">{label}</span>
      <span className="tabular text-right">{children}</span>
    </div>
  )
}

function Capability({ on, label }: { on: boolean; label: string }) {
  return <span className={`rounded-full border px-2 py-0.5 text-[11px] ${on ? "border-live/50 text-live" : "border-border text-faint line-through"}`}>{label}</span>
}

export function EngineView() {
  const { status, tick } = useLive()
  if (!status) return null
  const { provider, feed, engine } = status

  return (
    <div className="grid gap-3 lg:grid-cols-2">
      <Panel title="Provider">
        <Row label="Name">{provider.name}</Row>
        <Row label="Data">{provider.realtime ? "real-time" : `${provider.delay_seconds / 60}-minute delay`}</Row>
        <div className="flex flex-wrap gap-1.5 pt-2">
          <Capability on label="quotes" />
          <Capability on={provider.trades} label="trades" />
          <Capability on={provider.open_interest} label="open interest" />
          <Capability on={provider.vendor_greeks} label="vendor greeks" />
        </div>
      </Panel>

      <Panel title="Feed">
        <Row label="State">
          <FeedBadge state={tick?.feed.state ?? feed.state} message={feed.message} />
        </Row>
        <Row label="Last message">
          <span className="text-xs">{tick?.feed.message ?? feed.message}</span>
        </Row>
        <Row label="Updated">{clock(feed.updated)}</Row>
      </Panel>

      <Panel title="Engine">
        <Row label="Contracts tracked">{count(tick?.engine.contracts ?? engine.contracts)}</Row>
        <Row label="Events processed">{count(engine.events)}</Row>
        <Row label="Events per second">{fixed(tick?.engine.events_per_second ?? engine.events_per_second, 1)}</Row>
        <Row label="Last analytics pass">{fixed(tick?.engine.analytics_ms ?? engine.analytics_ms, 1)} ms</Row>
        <Row label="Uptime">{Math.floor(engine.uptime_seconds / 60)} min</Row>
      </Panel>

      <Panel title="Underlyings">
        <table className="w-full text-sm">
          <thead className="text-muted">
            <tr>
              <th className="py-1 text-left font-normal">Symbol</th>
              <th className="py-1 text-right font-normal">Spot</th>
              <th className="py-1 text-right font-normal">Expiries</th>
              <th className="py-1 text-right font-normal">Options priced</th>
              <th className="py-1 text-right font-normal">As of</th>
            </tr>
          </thead>
          <tbody className="tabular">
            {status.underlyings.map((u) => (
              <tr key={u.symbol} className="border-t border-border/40">
                <td className="py-1">{u.symbol}</td>
                <td className="py-1 text-right">{price(u.spot)}</td>
                <td className="py-1 text-right">{u.expiries}</td>
                <td className="py-1 text-right">{count(u.options)}</td>
                <td className="py-1 text-right"><AsOf asOf={u.as_of} delaySeconds={provider.delay_seconds} /></td>
              </tr>
            ))}
          </tbody>
        </table>
      </Panel>

      <Panel title="How the numbers are made" className="lg:col-span-2">
        <ul className="list-disc space-y-1.5 pl-5 text-sm text-muted">
          <li>
            Every provider is normalised into the same contracts, quotes, trades and open interest, so the analytics are identical whichever data
            you plug in. Where a provider publishes its own IVs, the chain shows how closely they agree.
          </li>
          <li>
            Each expiry's forward and discount factor come from a weighted put-call parity fit (C − P = D(F − K)) over near-the-money strikes; short
            expiries borrow the rate fitted on longer ones. No rates or dividend feed needed.
          </li>
          <li>
            Implied vols are solved with Newton's method on log price from a Corrado-Miller start, safeguarded by bisection; each strike's smile vol
            comes from its out-of-the-money side and drives both sides' Greeks.
          </li>
          <li>The C++20 engine re-prices every chain each second on the data's own clock, so delayed feeds get the right time to expiry.</li>
        </ul>
      </Panel>
    </div>
  )
}
