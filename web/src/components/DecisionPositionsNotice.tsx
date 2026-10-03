import type { Account, Portfolio } from "../api/trading-types"

/** A decision is final even when its market IOC could not finish liquidating. */
export function DecisionPositionsNotice({ account, portfolio }: { account?: Account; portfolio?: Portfolio }) {
  if (!account || account.evaluation.status === "active" || !portfolio ||
      (portfolio.positions.length === 0 && !portfolio.stocks?.length)) return null
  return <p role="status" className="rounded-md border border-warn/50 bg-warn/5 p-3 text-sm text-warn">
    Positions remain open after the evaluation {account.evaluation.status}. The system close keeps retrying and needs a fresh executable
    book, a bid or ask with closing liquidity, and the regular session. You may place closing orders, use Close all, or dispose of a
    worthless long with no bid. Opening orders require a new attempt. Later closes do not change this decision.
  </p>
}
