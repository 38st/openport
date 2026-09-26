import { useState, type ReactNode } from "react"
import { useLive } from "../api/live"
import { useAccount, usePlans, useRisk } from "../api/trading"
import type { Account, Plan, TradingStatus } from "../api/trading-types"
import { ResetDialog } from "../components/ResetDialog"
import { evaluationBadge } from "../components/Sidebar"
import { TradingError } from "../components/TradingControls"
import { Empty, PageHeader, Panel } from "../components/ui"
import { lockReason, offeredPlans, payoutCap } from "../lib/payouts"
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
  const p = r.payouts
  const funded = r.phase === "funded"
  return [
    funded ? { title: "Funded account", body: <>There is no profit target: trade the account and withdraw from its profits under the payout rules below.
        The account stays open until equity touches the drawdown floor.</> }
    : { title: "Profit target", body: r.profit_target
      ? <>Pass by reaching <strong className="text-foreground">{formatMoney(e.target_equity)}</strong> equity, {formatMoney(r.profit_target)} above your {formatMoney(e.starting_balance)} starting balance. There is no time limit and no minimum number of trading days. Once you pass, positions are closed and the attempt is complete.</>
      : "This account has no profit target." },
    { title: "Trailing drawdown", body: r.max_drawdown
      ? <>Equity may never touch the floor, now <strong className="text-foreground">{formatMoney(e.floor)}</strong>{e.floor_locked
          ? <>. It has locked at {formatMoney(r.lock_balance)} and no longer trails.</>
          : <>: {formatMoney(r.max_drawdown)} below your highest equity ({formatMoney(e.peak)}).
            {r.drawdown_mode === "intraday" ? " The floor rises with every new equity high during the session." : " The floor rises only once a day, from each day's closing equity."} It never moves down.
            {r.lock_balance && <> Once it reaches {formatMoney(r.lock_balance)} it locks there and stops trailing.</>}</>}{" "}
        Breaches are checked on every update in both modes; touching the floor {funded ? "closes the funded account" : "fails the attempt"} and closes every position.</>
      : "This account has no drawdown floor." },
    ...(p ? [{ title: "Payouts", body: <>
        A payout needs <strong className="text-foreground">{p.qualifying_days} qualifying days</strong> since the previous one: days that end with at least {formatMoney(p.qualifying_profit)} of net realised profit, after fees.
        Request it with no open positions or working orders. Each payout may take up to {p.withdrawal_percent}% of the profit above your {formatMoney(e.starting_balance)} starting balance,
        at least {formatMoney(p.minimum)}{p.caps.length ? <> and at most {p.caps.map((cap, i) => `${formatMoney(cap, 0)} for payout ${i + 1}${i === p.caps.length - 1 && i > 0 ? " and later" : ""}`).join(", ")}</> : null}; you keep {p.split_percent}%.
        Each finished day counts once, toward the payout cycle in progress when it closes.
        A withdrawal is not a loss: the day's starting equity {r.lock_balance ? "and a floor that has not locked yet move" : "moves"} down with it.
        {payoutCap(p.caps, e.payouts.length + 1) && <> Your next payout is number {e.payouts.length + 1}, capped at {formatMoney(payoutCap(p.caps, e.payouts.length + 1))}.</>}
      </> }] : []),
    { title: "Strategies", body: r.buy_only
      ? "Buy-only and single-leg: open positions by buying calls or puts. A sell may only close contracts you already hold, counting your other working sells. Multi-leg orders are not available."
      : r.defined_risk
        ? "Defined risk only: each short option needs a long of the same type on the same underlying that expires with it or later, so no position can lose without limit. Open spreads, condors and butterflies as one order from the Trade page's Strategy mode, or buy the long first; an order that would leave a short uncovered, now or once your open orders fill, is refused, and closing a short is always allowed."
        : "Any strategy: buy or sell calls and puts, or place spreads, straddles, condors and butterflies of up to four legs as one order from the Trade page's Strategy mode. All legs fill together at a net debit or credit." },
    { title: "Buying power", body: <>{r.buying_power
      ? <>Orders that use buying power must fit within it, now {formatMoney(account.buying_power.available)}: {r.margin === "portfolio"
          ? "equity (cash and the positions at their marks)" : "cash"}, less working orders' reservations, less the positions' margin requirement. </>
      : <>Buying power is not enforced; cash may go negative. </>}
      {r.margin === "portfolio"
        ? <>Portfolio margin: each underlying holds its largest loss across 11 price shocks, from −8% to +6% for index products and −15% to +15% for stocks and ETFs,
          or $37.50 per standard option contract, long or short, if that is larger. Options are repriced at unchanged volatility and time to expiry; shares move with the underlying.
          Long options and shares count as collateral, so you can borrow against them.</>
        : <>Strategy margin: long premium is paid in full.
        A naked short holds its buy-back value plus 100 × max(20% of spot − out-of-the-money amount, 10% of spot or strike). Spreads are netted: a vertical holds its width,
        an iron condor one wing, and a position with a bounded worst case at expiry never more than that loss.</>} Orders that free buying power are always allowed.</> },
    { title: "Expiring positions", body: minutes > 0
      ? `From ${minutes} minutes before a contract's last trade, working orders on it are cancelled, the position is closed at the bid or ask with the account's slippage, and only closing orders are accepted. Expiring index options such as SPXW and XSP last trade at 4:00 pm ET, SPY, QQQ, IWM, DIA and other ETF options at 4:15 pm, and AM-settled series at the regular close the day before.`
      : "Positions are held into expiry. Expiring index options trade until 4:00 pm ET and settle in cash; SPY, QQQ, IWM, DIA and other ETF options trade until 4:15 pm and deliver shares when a cent or more in the money at the 4:00 pm close." },
    { title: "Trading hours", body: "Every product trades in its regular session. SPX, XSP, VIX and RUT options also trade overnight, 8:15 pm to 9:25 am ET, and in the 4:15 to 5:00 pm curb session. Those sessions take plain limit orders only, a day order lasts until its session ends, and stops, triggered orders and the account's own closing orders wait for the regular session. A trading day ends at 5:00 pm ET, so an overnight trade counts toward the next day. A delayed or stalled feed refuses new orders rather than filling on stale quotes." },
    { title: "Fills", body: <>{r.slippage_ticks
      ? <>Slippage is {r.slippage_ticks} {r.slippage_ticks === 1 ? "tick" : "ticks"}: buys pay more than the ask and sells receive less than the bid, never below zero, using the displayed price's tick size. This applies to every leg, bracket exits and automatic closes. </>
      : <>Slippage is 0 ticks: buys fill at the ask and sells at the bid. </>}
      Fills use up to the displayed size. Single-leg fills stop at your limit; multi-leg orders wait if the slipped net exceeds the net limit.
      Unfilled day orders rest until the session ends. Each contract costs {fee ? formatMoney(fee) : "the configured fee"}.</> },
    { title: "Daily loss limit", body: dailyLoss ? <>Losing more than {formatMoney(dailyLoss)} from the day's starting equity trips the kill switch and cancels working orders.</> : "Set in Positions → Edit limits." },
    { title: "Exercise and assignment", body: "Equity and ETF options a cent or more in the money at expiry are exercised or assigned into 100 shares a contract, and long ones can be exercised early. A short option that trades below its exercise value at the close, such as a deep put, or a call whose time value is less than a dividend going ex the next day, can be assigned overnight, in part and at random, as real assignments are. Shares are marked at the underlying's price and can be sold or bought back in the regular session. Dividends are paid when the server knows them, from a dividend file or from Massive: on each ex-date, shares held into it receive the dividend and short shares pay it." },
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
      <TradingError error={plans.error} />
      {plans.data ? <>
        {/* Funded plans appear only when the terminal offers them (lib/features). */}
        <Panel title="Evaluation plans">
          <PlanTable label="Evaluation plans" plans={plans.data.plans.filter((p) => p.rules.phase !== "funded")} columns={[
            ["Profit target", (p) => p.rules.profit_target ? formatMoney(p.rules.profit_target, 0) : "—", true],
            ["Trailing drawdown", (p) => p.rules.max_drawdown ? formatMoney(p.rules.max_drawdown, 0) : "—", true],
            ["Floor moves", (p) => p.rules.max_drawdown ? (p.rules.drawdown_mode === "intraday" ? "Every new high" : "At each close") : "—"],
            ["Strategies", (p) => p.rules.buy_only ? "Buy only" : p.rules.defined_risk ? "Defined risk" : "Any"],
            ["Margin", (p) => p.rules.margin === "portfolio" ? "Portfolio" : "Strategy"],
            ["Slippage", (p) => `${p.rules.slippage_ticks ?? 0} ticks`],
            ["Expiry auto-close", (p) => p.rules.expiry_cutoff_seconds ? `${Math.round(p.rules.expiry_cutoff_seconds / 60)} min before` : "—"],
          ]} lock={() => null} enabled={trading.enabled} onStart={setStart} />
        </Panel>
        {offeredPlans(plans.data.plans).some((p) => p.rules.phase === "funded") && <Panel title="Funded accounts">
          <PlanTable label="Funded accounts" plans={offeredPlans(plans.data.plans).filter((p) => p.rules.phase === "funded")} columns={[
            ["Trailing drawdown", (p) => p.rules.max_drawdown
              ? `${formatMoney(p.rules.max_drawdown, 0)} ${p.rules.drawdown_mode === "intraday" ? "intraday" : "at close"}` : "—"],
            ["Floor locks at", (p) => p.rules.lock_balance ? formatMoney(p.rules.lock_balance, 0) : "—", true],
            ["Strategies", (p) => p.rules.buy_only ? "Buy only" : p.rules.defined_risk ? "Defined risk" : "Any"],
            ["Margin", (p) => p.rules.margin === "portfolio" ? "Portfolio" : "Strategy"],
            ["Slippage", (p) => `${p.rules.slippage_ticks ?? 0} ticks`],
            ["Payout after", (p) => p.rules.payouts ? `${p.rules.payouts.qualifying_days} days of ${formatMoney(p.rules.payouts.qualifying_profit, 0)}+` : "—"],
            ["Your share", (p) => p.rules.payouts ? `${p.rules.payouts.split_percent}%` : "—"],
          ]} lock={(p) => lockReason(p, plans.data.plans, data)} enabled={trading.enabled} onStart={setStart} />
        </Panel>}
      </> : !plans.error && <p className="text-sm text-muted">Loading plans…</p>}
      {start && <ResetDialog trading={trading} attempt={data.evaluation.attempt} initial={start} onClose={() => setStart(null)} />}
    </div>
  )
}

function PlanTable({ label, plans, columns, lock, enabled, onStart }: {
  /** Header, cell and whether the column holds figures. */
  label: string; plans: Plan[]; columns: [string, (plan: Plan) => ReactNode, boolean?][]
  lock: (plan: Plan) => string | null; enabled: boolean; onStart: (id: string) => void
}) {
  return (
    <div className="max-w-full overflow-x-auto" tabIndex={0} role="region" aria-label={label}>
      <table className="w-full text-left text-xs whitespace-nowrap">
        <thead className="text-[11px] uppercase tracking-wide text-muted"><tr>
          {["Plan", "Starting balance", ...columns.map(([header]) => header), ""].map((h) => <th key={h} className="px-2 py-2 font-normal">{h}</th>)}
        </tr></thead>
        <tbody>{plans.map((p) => {
          const locked = lock(p)
          return <tr key={p.id} className="border-t border-border/40">
            <td className="px-2 py-2"><div className="font-medium">{p.name}</div><div className="max-w-64 truncate text-[11px] text-muted" title={p.summary}>{p.summary}</div></td>
            <td className="px-2 py-2 tabular">{formatMoney(p.initial_cash, 0)}</td>
            {columns.map(([header, cell, numeric]) => <td key={header} className={`px-2 py-2 ${numeric ? "tabular" : ""}`}>{cell(p)}</td>)}
            <td className="px-2 py-2 text-right"><button type="button" className="trade-button" title={locked ?? undefined}
              disabled={!enabled || locked != null} onClick={() => onStart(p.id)}>{locked ? "Locked" : "Start"}</button></td>
          </tr>
        })}</tbody>
      </table>
    </div>
  )
}
