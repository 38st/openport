// @vitest-environment jsdom
import { QueryClient, QueryClientProvider } from "@tanstack/react-query"
import { act, type ReactNode } from "react"
import { createRoot, type Root } from "react-dom/client"
import { afterEach, beforeEach, expect, it, vi } from "vitest"
import { api } from "../api/client"
import { liveState, useLive } from "../api/live"
import type { Order } from "../api/trading-types"
import type { Chain, ChainRow } from "../api/types"
import { closingPlan, strategyGroups } from "../lib/positions"
import { account, order, portfolio, quote, selection, status, trading } from "../test/trading-fixtures"
import { RollDialog, SpreadExitsDialog } from "./StrategyActions"
import { StrategyTicket } from "./StrategyTicket"

vi.mock("../api/live", async (original) => ({ ...await original<typeof import("../api/live")>(), useLive: vi.fn() }))
vi.mock("../api/trading", async (original) => ({ ...await original<typeof import("../api/trading")>(), useRefreshTrading: () => vi.fn() }))
const positions = [
  { ...portfolio.positions[0]!, symbol: "SPXW  261016P06900000", strike: 6900, type: "put" as const, quantity: -2 },
  { ...portfolio.positions[0]!, symbol: "SPXW  261016P06890000", strike: 6890, type: "put" as const, quantity: 2 },
  { ...portfolio.positions[0]!, symbol: "SPXW  261016C07100000", strike: 7100, type: "call" as const, quantity: -2 },
  { ...portfolio.positions[0]!, symbol: "SPXW  261016C07110000", strike: 7110, type: "call" as const, quantity: 2 },
]
const entry: Order = { ...order, id: "5", symbol: null, side: null, filled_quantity: 2, status: "filled", average_fill_price: "-2.00",
  legs: positions.map((p) => ({ symbol: p.symbol, side: p.quantity > 0 ? "buy" : "sell", ratio: 1 })) }
const group = strategyGroups(positions, [entry], null)[0]!
const spread = strategyGroups(positions.slice(0, 2), [{ ...entry, legs: entry.legs!.slice(0, 2) }], null)[0]!
let root: Root, host: HTMLDivElement, client: QueryClient
beforeEach(() => {
  vi.stubGlobal("IS_REACT_ACT_ENVIRONMENT", true)
  vi.stubGlobal("ResizeObserver", class { observe() {} disconnect() {} })
  vi.mocked(useLive).mockReturnValue(liveState(status, null, "open"))
  vi.spyOn(api, "orders").mockResolvedValue({ account_version: "17", orders: [] })
  vi.spyOn(api, "submitOrder").mockResolvedValue({ account_version: "18", order, fills: [] })
  vi.spyOn(api, "account").mockResolvedValue({ ...account, rules: { ...account.rules, buy_only: false } })
  vi.spyOn(api, "portfolio").mockResolvedValue({ ...portfolio, positions })
  Object.defineProperty(HTMLElement.prototype, "scrollIntoView", { configurable: true, value: vi.fn() })
  Object.defineProperty(HTMLDialogElement.prototype, "showModal", { configurable: true, value() { this.open = true } })
  Object.defineProperty(HTMLDialogElement.prototype, "close", { configurable: true, value() { this.open = false } })
  host = document.createElement("div"); document.body.append(host); root = createRoot(host)
  client = new QueryClient({ defaultOptions: { queries: { retry: false, gcTime: Infinity } } })
})
afterEach(async () => {
  await act(async () => root.unmount()); client.clear(); host.remove(); vi.restoreAllMocks(); vi.unstubAllGlobals()
})
async function render(node: ReactNode) {
  await act(async () => root.render(<QueryClientProvider client={client}>{node}</QueryClientProvider>))
  await act(async () => { await new Promise((resolve) => setTimeout(resolve, 20)) })
}
async function click(text: string) {
  const button = [...host.querySelectorAll("button")].find((b) => b.textContent === text || b.getAttribute("aria-label") === text)
  if (!button) throw new Error(`Missing button ${text}`)
  await act(async () => button.click())
}
async function setField(name: string, value: string) {
  const element = [...host.querySelectorAll("label")].find((l) => l.textContent?.startsWith(name))?.querySelector("input, select")
  if (!(element instanceof HTMLInputElement || element instanceof HTMLSelectElement)) throw new Error(`Missing field ${name}`)
  await act(async () => {
    Object.getOwnPropertyDescriptor(element instanceof HTMLInputElement ? HTMLInputElement.prototype : HTMLSelectElement.prototype, "value")!.set!.call(element, value)
    element.dispatchEvent(new Event(element instanceof HTMLInputElement ? "input" : "change", { bubbles: true }))
  })
}
async function check(name: string) {
  const input = [...host.querySelectorAll("label")].find((l) => l.textContent === name)?.querySelector("input")
  if (!input) throw new Error(`Missing checkbox ${name}`)
  await act(async () => input.click())
}

it("sends GTC and template tags with a percentage target and combo stop", async () => {
  const legs = spread.legs.map(({ leg }, i) => ({ ...leg, quote: { ...quote, bid: i === 0 ? 5 : 3, ask: i === 0 ? 5.2 : 3.2, mid: i === 0 ? 5.1 : 3.1 } }))
  await render(<StrategyTicket legs={legs} onLegs={() => {}} expiries={[selection.expiry]} underlying="SPX" spot={7000} trading={trading}
    onClose={() => {}} tags={["put-credit-10d-5w"]} note="Template" />)
  await click("GTC")
  await check("Spread exits")
  await setField("Stop level", "4.00")
  await act(async () => host.querySelector("form")!.dispatchEvent(new Event("submit", { bubbles: true, cancelable: true })))
  expect(api.submitOrder).toHaveBeenCalledWith(expect.objectContaining({
    time_in_force: "gtc", tags: ["put-credit-10d-5w"], note: "Template",
    bracket: { take_profit: { limit_price: "1.00" }, stop_loss: { trigger: { source: "combo", direction: "at_or_above", level: "4.00" } } },
  }), trading.write)
})

it("sets exits on the held closing legs under the reduce-only latch", async () => {
  await render(<SpreadExitsDialog group={spread} trading={{ ...trading, kill_latched: true }} onClose={() => {}} />)
  await setField("Stop source", "underlying")
  await setField("Stop direction", "at_or_below")
  await setField("Stop level", "6800")
  await click("Set exits")
  expect(api.submitOrder).toHaveBeenCalledWith(expect.objectContaining({
    legs: closingPlan(spread).legs.map(({ symbol, side, ratio }) => ({ symbol, side, ratio })),
    quantity: 2, exits_only: true, type: "limit", time_in_force: "gtc", limit_price: "1.00",
    bracket: { take_profit: { limit_price: "1.00" }, stop_loss: { trigger: { source: "underlying", direction: "at_or_below", level: "6800" } } },
  }), trading.write)
})

it("changes a signed combo stop and cancels the held OCO exits", async () => {
  const legs = closingPlan(spread).legs.map(({ symbol, side, ratio }) => ({ symbol, side, ratio }))
  const stop: Order = { ...entry, id: "6", filled_quantity: 0, remaining_quantity: 2, legs, role: "stop_loss", status: "armed", type: "market",
    trigger: { source: "combo", direction: "at_or_above", level: "-0.50" }, oco: "7" }
  const target: Order = { ...entry, id: "7", filled_quantity: 0, remaining_quantity: 2, legs, role: "take_profit", status: "working", oco: "6" }
  vi.mocked(api.orders).mockResolvedValue({ account_version: "17", orders: [stop, target] })
  vi.spyOn(api, "modifyOrder").mockResolvedValue({ account_version: "18", order: stop, fills: [] })
  vi.spyOn(api, "cancelOrder").mockResolvedValue({ account_version: "19", order: stop })
  await render(<SpreadExitsDialog group={spread} trading={trading} onClose={() => {}} />)
  await click("Change")
  await setField("Trigger level", "-0.20")
  await act(async () => host.querySelector("form")!.dispatchEvent(new Event("submit", { bubbles: true, cancelable: true })))
  expect(api.modifyOrder).toHaveBeenCalledWith("6", { trigger_level: "-0.20" }, trading.write)
  await click("Cancel exits")
  expect(api.cancelOrder).toHaveBeenCalledWith("6", trading.write)
  expect(api.cancelOrder).toHaveBeenCalledWith("7", trading.write)
})

it("rolls the call side of a condor with new strikes as one four-leg order", async () => {
  const later = { ...selection.expiry, id: "2026-10-23PM", expiry: "2026-10-23" }
  const rows = (next: boolean): ChainRow[] => [6890, 6900, 7100, 7110, 7120, 7130].map((strike) => ({
    strike, iv: 0.2, gex: null, vex: null,
    call: { ...quote, symbol: `SPXW  2610${next ? "23" : "16"}C0${strike}000` },
    put: { ...quote, symbol: `SPXW  2610${next ? "23" : "16"}P0${strike}000` },
  }))
  vi.spyOn(api, "summary").mockResolvedValue({ expiries: [later] } as Awaited<ReturnType<typeof api.summary>>)
  vi.spyOn(api, "chain").mockImplementation(async (_underlying, id) => ({
    expiry: id === later.id ? later : { ...selection.expiry, id: group.expiry }, spot: 7000, strikes: rows(id === later.id),
  }) as Chain)
  await render(<RollDialog group={group} trading={trading} onClose={() => {}} />)
  await setField("Roll side", "call")
  await setField("New call strike (sell)", "7120")
  await setField("New call strike (buy)", "7130")
  await act(async () => host.querySelector("form")!.dispatchEvent(new Event("submit", { bubbles: true, cancelable: true })))
  const request = vi.mocked(api.submitOrder).mock.calls[0]![0]
  expect(request.legs).toEqual([
    { symbol: positions[2]!.symbol, side: "buy", ratio: 1 }, { symbol: positions[3]!.symbol, side: "sell", ratio: 1 },
    { symbol: "SPXW  261023C07120000", side: "sell", ratio: 1 }, { symbol: "SPXW  261023C07130000", side: "buy", ratio: 1 },
  ])
})

it("warns for the thin leg and shows per-leg market quantities and displayed sides", async () => {
  const legs = spread.legs.map(({ leg }, index) => ({ ...leg, side: index === 0 ? "sell" as const : "buy" as const,
    ratio: index === 0 ? 1 : 2, quote: { ...quote, bid: 5, ask: index === 0 ? 5.2 : 8, mid: index === 0 ? 5.1 : 6.5,
      volume: index === 0 ? 1000 : 12, oi: index === 0 ? 1000 : 34, bid_size: 3, ask_size: 7 } }))
  await render(<StrategyTicket legs={legs} onLegs={() => {}} expiries={[selection.expiry]} underlying="SPX" spot={7000} trading={trading} onClose={() => {}} />)
  expect(host.textContent).toContain(`${legs[1]!.strike} put: Thin liquidity`)
  expect(host.textContent).toContain("Volume 12 · OI 34")
  const submit = host.querySelector<HTMLButtonElement>('[aria-label="Submit strategy order"]')!
  expect(submit.disabled).toBe(false)
  await setField("Quantity (units)", "3")
  await click("Market")
  expect(host.textContent).toContain("3 contracts against displayed bid size 3")
  expect(host.textContent).toContain("6 contracts against displayed ask size 7")
  expect(host.textContent).toContain("Fills are simulated against the displayed quote and size only.")
  await click("Submit strategy order")
  expect(api.submitOrder).toHaveBeenCalledWith(expect.objectContaining({ type: "market", quantity: 3 }), trading.write)
})
