// @vitest-environment jsdom
import { QueryClient, QueryClientProvider } from "@tanstack/react-query"
import { act } from "react"
import { createRoot } from "react-dom/client"
import { expect, it, vi } from "vitest"
import { liveState, useLive } from "../api/live"
import { chain, selection, status, summary } from "../test/trading-fixtures"
import { ChainView } from "./ChainView"

vi.mock("../api/live", async (original) => ({ ...await original<typeof import("../api/live")>(), useLive: vi.fn() }))
vi.mock("../components/PriceChart", () => ({ PriceChart: () => null }))
vi.mock("../api/trading", () => ({ usePortfolio: () => ({ data: undefined }) }))

it("toggles liquidity columns independently of Greeks", async () => {
  vi.stubGlobal("IS_REACT_ACT_ENVIRONMENT", true)
  vi.mocked(useLive).mockReturnValue(liveState({ ...status, trading: undefined }, null, "open"))
  const client = new QueryClient({ defaultOptions: { queries: { retry: false, staleTime: Infinity, gcTime: Infinity } } })
  client.setQueryData(["summary", "SPX", 1], summary)
  client.setQueryData(["chain", "SPX", selection.expiry.id, .05, 1], chain)
  const host = document.createElement("div")
  const root = createRoot(host)
  try {
    await act(async () => root.render(<QueryClientProvider client={client}><ChainView symbol="SPX" expiry={selection.expiry.id} onExpiry={() => {}} /></QueryClientProvider>))
    const headers = () => [...host.querySelectorAll("th")].map((cell) => cell.textContent)
    const toggle = [...host.querySelectorAll("button")].find((button) => button.textContent === "Liquidity columns")!
    expect(headers().filter((name) => name === "Vol")).toHaveLength(2)
    await act(async () => toggle.click())
    expect(toggle.getAttribute("aria-pressed")).toBe("false")
    expect(headers()).not.toContain("Vol")
    expect(headers()).not.toContain("Spr %")
    expect(headers()).not.toContain("Liq")
    expect(headers()).toContain("OI")
    const greeks = [...host.querySelectorAll("button")].find((button) => button.textContent === "Greeks")!
    await act(async () => greeks.click())
    expect(headers()).toContain("Vega")
    expect(headers()).not.toContain("Vol")
    await act(async () => toggle.click())
    expect(headers()).toContain("Vega")
    expect(headers()).toContain("Vol")
  } finally {
    await act(async () => root.unmount())
    client.clear()
    vi.unstubAllGlobals()
  }
})
