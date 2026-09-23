import { QueryClient, QueryClientProvider } from "@tanstack/react-query"
import type { ReactNode } from "react"
import { renderToStaticMarkup } from "react-dom/server"
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest"
import { liveState, useLive } from "../api/live"
import { tradingQueries } from "../api/trading"
import type { Order, Position } from "../api/trading-types"
import { CancelAllDialog, EditOrderDialog, FlattenDialog } from "../components/OrderActions"
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
})
