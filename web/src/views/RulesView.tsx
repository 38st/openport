import { useState, type ReactNode } from "react"
import { useLive } from "../api/live"
import { useAccount, usePlans, useRisk } from "../api/trading"
import type { Account, TradingStatus } from "../api/trading-types"
import { ResetDialog } from "../components/ResetDialog"
import { evaluationBadge } from "../components/Sidebar"
import { TradingError } from "../components/TradingControls"
import { Empty, PageHeader, Panel } from "../components/ui"
import { formatMoney } from "../lib/trading"

function Rule({ title, children }: { title: string; children: ReactNode }) {
  return <div className="border-b border-border/50 py-3 last:border-0">
    <h3 className="text-sm font-medium">{title}</h3>
    <div className="mt-1 text-sm text-muted">{children}</div>
  </div>
}

/** Plain-language rules with the account's own numbers. */
export function ruleText(account: Account, fee?: string, dailyLoss?: string) {
  const r = account.rules
  const e = account.evaluation
  const minutes = Math.round(r.expiry_cutoff_seconds / 60)
  return [
    { title: "Profit target", body: r.profit_target
      ? <>Pass by reaching <strong className="text-foreground">{formatMoney(e.target_equity)}</strong> equity, {formatMoney(r.profit_target)} above your {formatMoney(e.starting_balance)} starting balance. There is no time limit and no minimum number of trading days. Once you pass, positions are closed and the attempt is complete.</>
      : "This account has no profit target." },
    { title: "Trailing drawdown", body: r.max_drawdown
      ? <>Equity may never touch the floor, now <strong className="text-foreground">{formatMoney(e.floor)}</strong>: {formatMoney(r.max_drawdown)} below your highest equity ({formatMoney(e.peak)}).
        {r.drawdown_mode === "intraday" ? " The floor rises with every new equity high during the session." : " The floor rises only once a day, from each day's closing equity."} It never moves down.
        Breaches are checked on every update in both modes; touching the floor fails the attempt and closes every position.</>
      : "This account has no drawdown floor." },
    { title: "Strategies", body: r.buy_only
      ? "Buy-only: open positions by buying calls or puts. A sell may only close contracts you already hold, counting your other working sells."
      : "Any single-leg strategy: buy or sell calls and puts. Short options hold a naked requirement against buying power." },
    { title: "Buying power", body: r.buying_power
      ? <>Orders that open contracts must fit within buying power, now {formatMoney(account.buying_power.available)}: cash, less working orders' reservations, less each short option's buy-back value plus 100 × max(20% of spot − out-of-the-money amount, 10% of spot or strike). Closing orders are always allowed.</>
      : "Buying power is not enforced; cash may go negative." },
    { title: "Expiring positions", body: minutes > 0
      ? `From ${minutes} minutes before expiry, working orders on a held contract are cancelled, the position is closed at the bid or ask, and only closing orders are accepted.`
      : "Positions are held into expiry and settle at intrinsic value." },
    { title: "Trading hours", body: "Orders are accepted and filled only during each product's regular session. A delayed or stalled feed refuses new orders rather than filling on stale quotes." },
    { title: "Fills", body: <>Buys fill at the ask and sells at the bid, up to the displayed size. Limit orders fill at your limit or better; unfilled day orders rest until the session ends. Each contract costs {fee ? formatMoney(fee) : "the configured fee"}.</> },
    { title: "Daily loss limit", body: dailyLoss ? <>Losing more than {formatMoney(dailyLoss)} from the day's starting equity trips the kill switch and cancels working orders.</> : "Set in Positions → Edit limits." },
  ]
}

export function RulesView() {
  const { trading, accountScope } = useLive()
  if (!trading) return null
  return <Rules key={accountScope} trading={trading} />
}

function Rules({ trading }: { trading: TradingStatus }) {
  const account = useAccount()
  const plans = usePlans()
  const risk = useRisk().data
  const [start, setStart] = useState<string | null>(null)
  const data = account.data
  if (account.error) return <TradingError error={account.error} />
  if (!data) return <Empty>Loading rules…</Empty>
  return (
    <div className="min-w-0 space-y-4">
      <PageHeader title="Rules" subtitle="What your account trades under, and the plans you can start" />
      <Panel title={<span className="flex items-center gap-2">{data.rules.plan ?? "Paper account"} {evaluationBadge(data, data.rules.plan)}</span>}>
        {ruleText(data, trading.fee_per_contract, risk?.limits.max_daily_loss).map((rule) => <Rule key={rule.title} title={rule.title}>{rule.body}</Rule>)}
      </Panel>
      <Panel title="Plans">
        <TradingError error={plans.error} />
        {plans.data ? <div className="max-w-full overflow-x-auto" tabIndex={0} role="region" aria-label="Plans">
          <table className="w-full text-left text-xs whitespace-nowrap">
            <thead className="text-[11px] uppercase tracking-wide text-muted"><tr>
              {["Plan", "Starting balance", "Profit target", "Trailing drawdown", "Floor moves", "Strategies", "Expiry auto-close", ""].map((h) => <th key={h} className="px-2 py-2 font-normal">{h}</th>)}
            </tr></thead>
            <tbody>{plans.data.plans.map((p) => <tr key={p.id} className="border-t border-border/40">
              <td className="px-2 py-2"><div className="font-medium">{p.name}</div><div className="max-w-72 truncate text-[11px] text-muted" title={p.summary}>{p.summary}</div></td>
              <td className="px-2 py-2 tabular">{formatMoney(p.initial_cash, 0)}</td>
              <td className="px-2 py-2 tabular">{p.rules.profit_target ? formatMoney(p.rules.profit_target, 0) : "—"}</td>
              <td className="px-2 py-2 tabular">{p.rules.max_drawdown ? formatMoney(p.rules.max_drawdown, 0) : "—"}</td>
              <td className="px-2 py-2">{p.rules.max_drawdown ? (p.rules.drawdown_mode === "intraday" ? "Every new high" : "At each close") : "—"}</td>
              <td className="px-2 py-2">{p.rules.buy_only ? "Buy only" : "Any"}</td>
              <td className="px-2 py-2">{p.rules.expiry_cutoff_seconds ? `${Math.round(p.rules.expiry_cutoff_seconds / 60)} min before` : "—"}</td>
              <td className="px-2 py-2 text-right"><button type="button" className="trade-button" disabled={!trading.enabled} onClick={() => setStart(p.id)}>Start</button></td>
            </tr>)}</tbody>
          </table>
        </div> : <p className="text-sm text-muted">Loading plans…</p>}
      </Panel>
      {start && <ResetDialog trading={trading} attempt={data.evaluation.attempt} initial={start} onClose={() => setStart(null)} />}
    </div>
  )
}
