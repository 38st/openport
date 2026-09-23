import { QueryClient, QueryClientProvider } from "@tanstack/react-query"
import type { ReactNode } from "react"
import { renderToStaticMarkup } from "react-dom/server"
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest"
import { ApiError } from "../api/client"
import { liveState, useLive } from "../api/live"
import { tradingQueries } from "../api/trading"
import { Header } from "../components/Header"
import { KillSwitch } from "../components/KillSwitch"
import { LimitsEditor } from "../components/LimitsEditor"
import { OrderResult, OrderTicket } from "../components/OrderTicket"
import { ScenarioGrid } from "../components/ScenarioGrid"
import { formatRoute, parseRoute, views } from "../lib/route"
import { chain, fill, order, portfolio, quote, risk, selection, status, summary, trading } from "../test/trading-fixtures"
import { ChainView } from "./ChainView"
import { PortfolioView } from "./PortfolioView"

vi.mock("../api/live", async (original) => ({ ...await original<typeof import("../api/live")>(), useLive: vi.fn() }))
vi.mock("../lib/theme", () => ({ useTheme: () => ({ theme: "dark", toggleTheme: vi.fn() }) }))
const clients: QueryClient[] = []
function render(node: ReactNode, seed?: (client: QueryClient) => void) {
  const client = new QueryClient({ defaultOptions: { queries: { retry: false, staleTime: Infinity, gcTime: Infinity } } })
  clients.push(client)
  const queries = tradingQueries(0, "17", true)
  client.setQueryData(queries.portfolio.queryKey, portfolio)
  client.setQueryData(queries.orders.queryKey, { account_version: "17", orders: [order] })
  client.setQueryData(queries.fills.queryKey, { account_version: "17", fills: [fill] })
  client.setQueryData(queries.risk.queryKey, risk)
  client.setQueryData(["summary", "SPX", 1], summary)
  client.setQueryData(["chain", "SPX", selection.expiry.id, .05, 1], chain)
  seed?.(client)
  return renderToStaticMarkup(<QueryClientProvider client={client}>{node}</QueryClientProvider>)
}
beforeEach(() => vi.mocked(useLive).mockReturnValue(liveState(status, null, "open")))
afterEach(() => { clients.splice(0).forEach((client) => client.clear()); vi.clearAllMocks() })

describe("paper trading fixtures", () => {
  it("renders a labelled buy ticket with quotes, sizes, exact premium and its own Greeks", () => {
    const html = render(<OrderTicket selection={selection} quote={quote} trading={trading} onClose={() => {}} />)
    expect(html).toContain("<dialog")
    expect(html).toContain("aria-labelledby=")
    expect(html).toContain("2026-10-16 PM")
    expect(html).toContain("7000 call")
    expect(html).toContain('<option value="buy" selected="">Buy</option>')
    expect(html).toContain("4.50 × 10")
    expect(html).toContain("4.60 × 3")
    expect(html).toContain("4.55")
    expect(html).toContain("$460.00")
    expect(html).toContain("50.00")
    expect(html).toContain("0.2000")
    expect(html).toContain("1225.00")
    expect(html).toContain("-85.00")
    expect(html).toContain("Not provided by server")
    expect(html).toContain("Submit paper order")
  })
  it("renders sells, null quotes and token/kill write gates", () => {
    const html = render(<OrderTicket selection={{ ...selection, cell: "bid", price: "4.50" }} quote={{ ...quote, bid: null, ask: null, mid: null, bid_size: null, ask_size: null, delta: null }} trading={{ ...trading, write: "token", kill_latched: true }} onClose={() => {}} />)
    expect(html).toContain('<option value="sell" selected="">Sell</option>')
    expect(html).toContain("— × —")
    expect(html).toContain("Estimated premium · credit")
    expect(html).toContain("Enter write token")
    expect(html).toContain("Kill switch latched")
    expect(html).toMatch(/<button[^>]*disabled=""[^>]*>Submit paper order/)
    expect(html).not.toContain("NaN")
  })
  it.each(["0.65", "0.00"])("uses the server fee %s and removes the manual override", (fee) => {
    const html = render(<OrderTicket selection={{ ...selection, price: "15.8" }} quote={quote} trading={{ ...trading, fee_per_contract: fee, initial_cash: "100000.00" }} onClose={() => {}} />)
    expect(html).toContain(`$${fee}`)
    expect(html).not.toContain("Not provided by server")
    expect(html).not.toContain("Fee / contract ($, estimate)")
    expect(html).toContain('value="15.80"')
    expect(html).toContain("Increase limit price one tick")
    expect(html).toContain("Decrease limit price one tick")
  })
  it.each(["global", "curb", "closed"] as const)("shows the %s session gate in the ticket and Portfolio", (name) => {
    const session = { name, open: name !== "closed", note: "Product session" }
    vi.mocked(useLive).mockReturnValue(liveState({ ...status, underlyings: [{ ...status.underlyings[0]!, session }] }, null, "open"))
    const html = render(<OrderTicket selection={selection} quote={quote} trading={trading} onClose={() => {}} />)
    expect(html).toContain("Paper orders are accepted in the regular session only; SPX is")
    expect(html).toContain(name === "global" ? "overnight session" : name)
    expect(html).toMatch(/<button[^>]*disabled=""[^>]*>Submit paper order/)
    expect(render(<PortfolioView />)).toContain("Paper orders are accepted in the regular session only; SPX is")
  })
  it("uses the selected underlying's regular session, even if another market is closed", () => {
    vi.mocked(useLive).mockReturnValue(liveState({ ...status, underlyings: [
      { ...status.underlyings[0]!, symbol: "SPY", session: { name: "closed", open: false, note: "Closed" } },
      { ...status.underlyings[0]!, session: { name: "regular", open: true, note: "Regular" } },
    ] }, null, "open"))
    const html = render(<OrderTicket selection={selection} quote={quote} trading={trading} onClose={() => {}} />)
    expect(html).not.toContain("regular session only")
    expect(html).not.toMatch(/<button[^>]*disabled=""[^>]*>Submit paper order/)
  })
  it.each(["working", "partially_filled", "filled", "rejected", "cancelled"] as const)("renders inline order status %s and typed risk rejections", (orderStatus) => {
    const html = render(<OrderResult order={{ ...order, status: orderStatus, reason: { code: "QUOTE_STALE", message: "Quote is too old" } }} error={new ApiError(422, "Delta cap exceeded", "DELTA_LIMIT", 200, 100, "aggregate")} />)
    expect(html).toContain(orderStatus === "partially_filled" ? "Partial fill" : orderStatus)
    expect(html).toContain("QUOTE_STALE")
    expect(html).toContain("DELTA_LIMIT")
    expect(html).toContain("Actual 200.00 · Limit 100.00 · aggregate")
  })
  it("renders portfolio values, awaiting settlement, open orders, fills and risk ranges", () => {
    const html = render(<PortfolioView />)
    for (const text of ["Paper portfolio", "$99,988.70", "−$11.30", "Valuation incomplete", "Awaiting settlement", "Age unavailable", "$4.55", "2 s old", "Partial fill", "Cancel", "$1.30", "Reachable including open orders", "1750000.00", "87.5%", "Spot × volatility scenarios", "Armed", "Edit limits"]) expect(html).toContain(text)
    expect(html).toContain("overflow-x-auto")
    expect(html).not.toContain("NaN")
    expect(html).not.toContain("undefined")
  })
  it("renders complete and empty portfolio fixtures", () => {
    const html = render(<PortfolioView />, (client) => {
      const queries = tradingQueries(0, "17", true)
      client.setQueryData(queries.portfolio.queryKey, { ...portfolio, valuation_complete: true, quality_flags: [], positions: [] })
      client.setQueryData(queries.orders.queryKey, { account_version: "17", orders: [] })
      client.setQueryData(queries.fills.queryKey, { account_version: "17", fills: [] })
    })
    expect(html).toContain("No positions")
    expect(html).toContain("No open orders")
    expect(html).toContain("No fills yet")
    expect(html).not.toContain("Valuation incomplete")
  })
  it("keeps null heatmap cells distinct from the exact unchanged-spot/vol zero", () => {
    const html = render(<ScenarioGrid scenarios={risk.scenarios} />)
    expect(html).toContain('title="0% spot, 0 vol points: 0 · unchanged spot and vol"')
    expect(html).toContain("$0.00")
    expect(html).toContain("unavailable · volatility clamped")
    expect(html).toContain("Scenario valuation incomplete")
    expect(html).toContain("Loss")
    expect(html).toContain("Gain")
  })
  it("renders a revision-based limits editor and a clear latched kill state", () => {
    const html = render(<LimitsEditor initial={risk} trading={trading} onClose={() => {}} />)
    expect(html).toContain("Editing revision 3")
    expect(html).toContain('value="5000.00"')
    expect(html).toContain("Relative price band (ratio)")
    const killHtml = render(<KillSwitch kill={{ latched: true, reason: "Daily loss reached" }} trading={trading} />)
    expect(killHtml).toContain("LATCHED · new orders blocked")
    expect(killHtml).toContain("Daily loss reached")
    expect(killHtml).toContain("Reset kill switch…")
  })
  it("exposes call and put bid/ask buttons and an untradable reason", () => {
    const html = render(<ChainView symbol="SPX" expiry={selection.expiry.id} onExpiry={() => {}} />)
    for (const label of ["sell 7000 call at bid", "buy 7000 call at ask", "sell 7000 put at bid", "buy 7000 put at ask"]) expect(html).toContain(label)
    const untradable = render(<ChainView symbol="SPX" expiry={selection.expiry.id} onExpiry={() => {}} />, (client) => client.setQueryData(["chain", "SPX", selection.expiry.id, .05, 1], { ...chain, strikes: [{ ...chain.strikes[0], call: { ...quote, tradable: false, untradable_reason: "AMERICAN_UNSUPPORTED" } }] }))
    expect(untradable).toContain("AMERICAN_UNSUPPORTED")
  })
  it("hides every trading entry point on older servers, even with cached trading payloads", () => {
    vi.mocked(useLive).mockReturnValue(liveState({ ...status, trading: undefined }, null, "open"))
    const header = render(<Header symbol="SPX" view="chain" onSymbol={() => {}} onView={() => {}} />)
    expect(header).not.toContain("Portfolio")
    expect(render(<PortfolioView />)).toBe("")
    const html = render(<ChainView symbol="SPX" expiry={selection.expiry.id} onExpiry={() => {}} />)
    expect(html).not.toContain("paper order")
    expect(html).not.toContain("at bid")
  })
  it("adds Portfolio as view five with hash routing", () => {
    expect(views[4]).toBe("portfolio")
    expect(parseRoute("#/SPX/portfolio").view).toBe("portfolio")
    expect(formatRoute({ view: "portfolio", symbol: "SPX", expiry: null })).toBe("#/SPX/portfolio")
    expect(render(<Header symbol="SPX" view="portfolio" onSymbol={() => {}} onView={() => {}} />)).toContain('title="Portfolio (5)"')
  })
})
