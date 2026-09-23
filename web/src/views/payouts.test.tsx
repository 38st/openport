import { QueryClient, QueryClientProvider } from "@tanstack/react-query"
import type { ReactNode } from "react"
import { renderToStaticMarkup } from "react-dom/server"
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest"
import { liveState, useLive } from "../api/live"
import { tradingQueries } from "../api/trading"
import type { Account } from "../api/trading-types"
import { ResetDialog } from "../components/ResetDialog"
import { account, fundedAccount, plans, portfolio, risk, status, trades } from "../test/trading-fixtures"
import { DashboardView } from "./DashboardView"
import { PayoutsView } from "./PayoutsView"
import { RulesView, ruleText } from "./RulesView"

vi.mock("../api/live", async (original) => ({ ...await original<typeof import("../api/live")>(), useLive: vi.fn() }))
const clients: QueryClient[] = []
function render(node: ReactNode, value: Account) {
  const client = new QueryClient({ defaultOptions: { queries: { retry: false, staleTime: Infinity, gcTime: Infinity } } })
  clients.push(client)
  const queries = tradingQueries(0, "17", true)
  client.setQueryData(queries.account.queryKey, value)
  client.setQueryData(queries.portfolio.queryKey, portfolio)
  client.setQueryData(queries.risk.queryKey, risk)
  client.setQueryData(queries.trades("current").queryKey, { account_version: "17", attempt: value.evaluation.attempt, trades })
  client.setQueryData(["plans"], { plans })
  return renderToStaticMarkup(<QueryClientProvider client={client}>{node}</QueryClientProvider>)
}
const passed: Account = { ...account, evaluation: { ...account.evaluation, status: "passed", decided_at: "2026-09-23T15:00:00Z",
  decided_equity: "110000.00", decision: "Equity $110000.00 reached the profit target $110000.00" } }
const trading = { ...status.trading!, enabled: true, write: "open" as const }
beforeEach(() => vi.mocked(useLive).mockReturnValue(liveState(status, null, "open")))
afterEach(() => { clients.splice(0).forEach((client) => client.clear()); vi.clearAllMocks() })

describe("funded accounts and payouts", () => {
  it("shows a funded account's payout standing, cycle and history", () => {
    const html = render(<PayoutsView />, fundedAccount)
    for (const text of [">funded<", "Request payout #2", "Not enough qualifying days in this payout cycle", "Qualifying days", "3 / 8",
      "+$6,050.00", "50% withdrawable: $3,025.00", "$1,600.00", "1 payout · $2,000.00 withdrawn", "3 of 8", ">Tue, Sep 22</td>",
      "$3,000.00 of $1,000.00", "Wed, Sep 23", "Thu, Sep 24", "+$300.00", "+$250.00", "Payout history", "Your share (80%)", "$107,500.00",
      "Cap this payout", "Locked at $100,000.00", "Not eligible yet"])
      expect(html).toContain(text)
    expect(html).toContain("payout cycle since Tue, Sep 22, 2026, 14:00 ET")
    expect(html).not.toContain("NaN")
    expect(html).toMatch(/<button type="submit" class="trade-button" disabled="">Request payout/)
  })
  it("prefills the maximum and previews the trader's share when eligible", () => {
    const eligible: Account = { ...fundedAccount, payout: { ...fundedAccount.payout!, eligible: true, blocked: null, qualifying_days: 8 } }
    const html = render(<PayoutsView />, eligible)
    expect(html).toContain('value="3000.00"')
    expect(html).toContain("Between $1,000.00 and $3,000.00. You receive $2,400.00 (80%).")
    expect(html).toContain("$2,400.00 to you (80%)")
    expect(html).toMatch(/<button type="submit" class="trade-button">Request payout/)
  })
  it("explains the path to a payout before the funded phase and offers an unlocked plan", () => {
    const html = render(<PayoutsView />, account)
    for (const text of ["Payouts are available on funded accounts", "You are on Intraday 100K (active)", "How payouts work", "Funded plans",
      "Funded Intraday 100K", "8 × $200+", "Pass Intraday 100K to unlock", "$2,000 / $3,000 / $4,000 / $6,000"])
      expect(html).toContain(text)
    const unlocked = render(<PayoutsView />, passed)
    expect(unlocked).toContain("Funded account unlocked")
    expect(unlocked).toContain("Start Funded Intraday 100K")
  })
  it("adds payout progress to the dashboard and the unlock to a passed evaluation", () => {
    const html = render(<DashboardView />, fundedAccount)
    for (const text of ["Next payout", "3 / 8 days", "Qualifying days · $200+ net each", "Qualifying days this cycle", "Next payout, once eligible", "up to $3,000.00",
      "Funded: no profit target", "locks at $100,000", "Payout every 8 days of $200+ net profit", "Up to 50% of profit per payout, 80% to you",
      "Drawdown floor (locked)", "buffer · locked"])
      expect(html).toContain(text)
    expect(html).not.toContain("Remaining to target")
    const closed = render(<DashboardView />, { ...fundedAccount, evaluation: { ...fundedAccount.evaluation, status: "failed" } })
    expect(closed).toContain("Funded account closed")
    const pass = render(<DashboardView />, passed)
    expect(pass).toContain("Evaluation passed")
    expect(pass).toContain("Start Funded Intraday 100K")
  })
  it("states the funded rules and locks funded plans until their evaluation passes", () => {
    const texts = ruleText(fundedAccount, "0.65", "5000.00")
    expect(texts.map((t) => t.title).slice(0, 3)).toEqual(["Funded account", "Trailing drawdown", "Payouts"])
    const [, drawdown, payout] = texts.map((rule) => renderToStaticMarkup(<>{rule.body}</>))
    expect(drawdown).toContain("It has locked at $100,000.00 and no longer trails.")
    expect(drawdown).toContain("closes the funded account")
    for (const text of ["8 qualifying days", "$200.00", "up to 50%", "$2,000 for payout 1", "$6,000 for payout 4 and later", "you keep 80%",
      "Each finished day counts once", "Your next payout is number 2, capped at $3,000.00."])
      expect(payout).toContain(text)
    const rules = render(<RulesView />, account)
    expect(rules).toContain("Pass Intraday 100K to unlock")
    expect(rules).toContain(">Locked</button>")
    for (const text of ["Evaluation plans", "Funded accounts", "Floor locks at", "8 days of $200+", "$5,000 intraday"]) expect(rules).toContain(text)
  })
  it("groups plans and enables the funded one only after a pass", () => {
    const locked = render(<ResetDialog trading={trading} attempt={2} onClose={() => {}} />, account)
    expect(locked).toContain("Evaluations")
    expect(locked).toContain("Funded accounts")
    expect(locked).toContain("Pass Intraday 100K to unlock")
    const radio = (html: string) => html.match(/<input[^>]*value="funded-intraday-100k"[^>]*>/)?.[0] ?? ""
    expect(radio(locked)).toContain('disabled=""')
    const open = render(<ResetDialog trading={trading} attempt={2} initial="funded-intraday-100k" onClose={() => {}} />, passed)
    expect(open).toContain(">unlocked<")
    expect(radio(open)).toContain('checked=""')
    expect(radio(open)).not.toContain("disabled")
    expect(open).toContain("Start Funded Intraday 100K")
  })
})
