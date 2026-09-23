import { QueryClient, QueryClientProvider } from "@tanstack/react-query"
import type { ReactNode } from "react"
import { renderToStaticMarkup } from "react-dom/server"
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest"
import { liveState, useLive } from "../api/live"
import { tradingQueries } from "../api/trading"
import type { Order, Position, StockHolding } from "../api/trading-types"
import { CancelAllDialog, EditOrderDialog, FlattenDialog } from "../components/OrderActions"
import { ExerciseDialog } from "../components/StockActions"
import { account, fill, order, portfolio, risk, status, trading } from "../test/trading-fixtures"
import { OrdersView } from "./OrdersView"
import { PositionsView } from "./PositionsView"

vi.mock("../api/live", async (original) => ({ ...await original<typeof import("../api/live")>(), useLive: vi.fn() }))
const clients: QueryClient[] = []
function render(node: ReactNode, orders: Order[] = [order]) {
  const client = new QueryClient({ defaultOptions: { queries: { retry: false, staleTime: Infinity, gcTime: Infinity } } })
  clients.push(client)
  const queries = tradingQueries(0, "17", true)
  client.setQueryData(queries.portfolio.queryKey, portfolio)
  client.setQueryData(queries.orders.queryKey, { account_version: "17", orders: orders.filter((o) => o.status !== "filled") })
  client.setQueryData(queries.allOrders.queryKey, { account_version: "17", orders })
  client.setQueryData(queries.fills.queryKey, { account_version: "17", fills: [fill] })
  client.setQueryData(queries.risk.queryKey, risk)
  client.setQueryData(queries.account.queryKey, account)
  return renderToStaticMarkup(<QueryClientProvider client={client}>{node}</QueryClientProvider>)
}
beforeEach(() => vi.mocked(useLive).mockReturnValue(liveState(status, null, "open")))
afterEach(() => { clients.splice(0).forEach((client) => client.clear()); vi.clearAllMocks() })

const stop: Order = { ...order, id: "9", status: "armed", role: "stop_loss", side: "sell", type: "market", time_in_force: "ioc",
  limit_price: null, quantity: 2, filled_quantity: 0, remaining_quantity: 2, parent: "order-1",
  trigger: { source: "option", direction: "at_or_below", level: "3.50" } }

describe("shares from exercise and assignment", () => {
  const spyCall: Position = { ...portfolio.positions[0]!, symbol: "SPY   261022C00500000", underlying: "SPY", strike: 500, type: "call",
    quantity: 2, mark: "21.10" }
  const shares: StockHolding = { symbol: "SPY", shares: -100, average_price: "510.00", basis: "-51000.00", mark: "505.00",
    mark_time: "2026-09-23T15:00:00.000Z", market_value: "-50500.00", unrealised: "500.00", realised: "0.00", fees: "0.00",
    fresh: true, attribution: { delta: 500, gamma: 0, vega: 0, theta: 0, other: 0, costs: 0, total: 500 } }
  it("lists the shares, and offers exercise on equity options but not index ones", () => {
    const client = new QueryClient({ defaultOptions: { queries: { retry: false, staleTime: Infinity, gcTime: Infinity } } })
    clients.push(client)
    const queries = tradingQueries(0, "17", true)
    client.setQueryData(queries.portfolio.queryKey, { ...portfolio, positions: [portfolio.positions[0]!, spyCall], stocks: [shares] })
    client.setQueryData(queries.orders.queryKey, { account_version: "17", orders: [] })
    client.setQueryData(queries.allOrders.queryKey, { account_version: "17", orders: [] })
    client.setQueryData(queries.risk.queryKey, risk)
    client.setQueryData(queries.account.queryKey, account)
    const html = renderToStaticMarkup(<QueryClientProvider client={client}><PositionsView /></QueryClientProvider>)
    expect(html).toContain("Shares · 1")
    expect(html).toContain("-100 short")
    expect(html).toContain("$510.00")
    expect(html).toContain('aria-label="Close SPY shares"')
    expect(html).toContain('aria-label="Exercise SPY   261022C00500000"')
    expect(html).not.toContain(`aria-label="Exercise ${portfolio.positions[0]!.symbol}"`)
  })
  it("explains an exercise: intrinsic value, the shares and the time value given up", () => {
    vi.mocked(useLive).mockReturnValue(liveState({ ...status, underlyings: [{ ...status.underlyings[0]!, symbol: "SPY", spot: 520 }] }, null, "open"))
    const html = render(<ExerciseDialog position={spyCall} trading={trading} onClose={() => {}} />)
    expect(html).toContain("close at intrinsic value, $20.00 with SPY at 520.00")
    expect(html).toContain("buy 200 SPY shares at that price: together, the $500.00 strike")
    expect(html).toContain("gives up about $220.00 of time value")
    expect(html).toContain(">Exercise 2</button>")
  })
})

describe("working orders", () => {
  it("offer an edit for resting orders and one button to cancel them all", () => {
    const html = render(<OrdersView />, [order, stop, { ...order, id: "12", status: "filled", filled_quantity: 5, remaining_quantity: 0 }])
    expect(html).toContain('aria-label="Edit order order-1 for SPX Oct 16 7000C"')
    expect(html).toContain('aria-label="Edit order 9 for SPX Oct 16 7000C"')
    expect(html).not.toContain('aria-label="Edit order 12')
    expect(html).toContain(">Cancel all</button>")
  })

  it("edit a limit order's size and price, keeping what already filled", () => {
    const html = render(<EditOrderDialog order={order} trading={trading} onClose={() => {}} onDone={() => {}} />)
    expect(html).toContain("Edit order #order-1")
    expect(html).toContain("Quantity, including 2 filled")
    expect(html).toContain('value="5"')
    expect(html).toContain("Limit price")
    expect(html).toContain('value="4.60"')
    expect(html).not.toContain("Trigger level")
    expect(html).toContain("No changes</button>")
  })

  it("move a stop's trigger but not its size", () => {
    const html = render(<EditOrderDialog order={stop} trading={trading} onClose={() => {}} onDone={() => {}} />)
    expect(html).toContain("Trigger level: activates when bid ≤ $3.50")
    expect(html).toContain("A bracket exit&#x27;s size follows the position it protects.")
    expect(html).not.toContain("Quantity")
    expect(html).not.toContain("Limit price")
  })

  it("warn before cancelling an exit that protects a position", () => {
    const html = render(<CancelAllDialog orders={[order, stop]} trading={trading} onClose={() => {}} onDone={() => {}} />)
    expect(html).toContain("This cancels 2 open orders, armed ones included.")
    expect(html).toContain("1 bracket exit protects a position; the position stays open without it.")
    expect(html).toContain("Cancel 2 orders</button>")
  })
})

describe("closing positions", () => {
  const short: Position = { ...portfolio.positions[0]!, symbol: "SPXW  261016P06900000", type: "put", strike: 6900, quantity: -1 }

  it("is one button on the positions page", () => {
    expect(render(<PositionsView />)).toContain(">Close all</button>")
  })

  it("lists what closes, short positions first, and what is cancelled", () => {
    const html = render(<FlattenDialog positions={[...portfolio.positions, short]} orders={[order, stop]} trading={trading} onClose={() => {}} />)
    expect(html).toContain("Close all positions")
    const buy = html.indexOf("Buy 1 at market")
    const sell = html.indexOf("Sell 2 at market")
    expect(buy).toBeGreaterThan(0)
    expect(sell).toBeGreaterThan(buy)
    expect(html).toContain("2 working orders are cancelled first.")
    expect(html).toContain("1 expired position waits for settlement.")
    expect(html).toContain("Close 2 positions</button>")
  })

  it("can start on one underlying", () => {
    const html = render(<FlattenDialog positions={[portfolio.positions[0]!]} orders={[]} trading={trading} initial="SPX" onClose={() => {}} />)
    expect(html).toContain("Flatten SPX")
    expect(html).toContain("Close 1 position</button>")
  })

  it("closes shares with the options", () => {
    const shares: StockHolding = { symbol: "SPY", shares: 200, average_price: "520.00", basis: "104000.00", mark: "525.00",
      mark_time: "2026-09-23T15:00:00.000Z", market_value: "105000.00", unrealised: "1000.00", realised: "0.00", fees: "0.00",
      fresh: true, attribution: null }
    const html = render(<FlattenDialog positions={[portfolio.positions[0]!]} stocks={[shares]} orders={[]} trading={trading} onClose={() => {}} />)
    expect(html).toContain("200 SPY shares")
    expect(html).toContain("Sell at the price")
    expect(html).toContain("Close 2 positions</button>")
  })

  it("waits for the regular session, since it sends market orders", () => {
    vi.mocked(useLive).mockReturnValue(liveState({ ...status, underlyings: [{ ...status.underlyings[0]!,
      session: { name: "global", open: true, note: "overnight session" } }] }, null, "open"))
    const html = render(<FlattenDialog positions={[portfolio.positions[0]!]} orders={[]} trading={trading} initial="SPX" onClose={() => {}} />)
    expect(html).toContain("SPX is outside the regular session, which takes limit orders only.")
    expect(html).toMatch(/disabled="">Close 1 position<\/button>/)
  })
})
