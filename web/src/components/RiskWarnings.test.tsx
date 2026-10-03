import { QueryClient, QueryClientProvider } from "@tanstack/react-query"
import { renderToStaticMarkup } from "react-dom/server"
import { describe, expect, it, vi } from "vitest"
import { liveState, useLive } from "../api/live"
import type { RiskWarning } from "../api/trading-types"
import { risk, status } from "../test/trading-fixtures"
import { RiskPanel } from "./RiskPanel"
import { RiskWarnings } from "./RiskWarnings"

vi.mock("../api/live", async (original) => ({ ...await original<typeof import("../api/live")>(), useLive: vi.fn() }))
const delivery: RiskWarning = { code: "EXPIRY_DELIVERY", severity: "warning", scope: "SPY", symbol: "SPY   260922C00500000",
  message: "2 long SPY 2026-09-22 500C expiring today are $10.00 in the money", actual: -92041.3, limit: 0 }
const ratchet: RiskWarning = { code: "FLOOR_RATCHET", severity: "info", scope: "aggregate", symbol: null,
  message: "Closing at today's equity $10578.70 raises the floor tonight from $9000.00 to $9578.70", actual: 9578.7, limit: 9000 }

describe("risk warnings", () => {
  it("lists each warning with its title, scope and the server's words, urgent ones as alerts", () => {
    const html = renderToStaticMarkup(<RiskWarnings warnings={[delivery, ratchet]} />)
    expect(html).toContain("Delivery at expiry")
    expect(html).toContain("SPY</span>")
    expect(html).toContain("are $10.00 in the money.")
    expect(html).toContain('role="alert"')
    expect(html).toContain("Floor rises tonight")
    expect(html).toContain("Account</span>")
    expect(html).toContain('role="status"')
  })
  it("shows an unused percent floor as informational", () => {
    const html = renderToStaticMarkup(<RiskWarnings warnings={[{ code: "SOFT_FLOOR_UNUSED", severity: "info", scope: "aggregate", symbol: null,
      message: "This plan has no drawdown floor", actual: null, limit: null }]} />)
    expect(html).toContain("Percent floor unused")
    expect(html).toContain('role="status"')
    expect(html).not.toContain('role="alert"')
  })
  it("shows nothing without warnings, as from an older server", () => {
    expect(renderToStaticMarkup(<RiskWarnings warnings={[]} />)).toBe("")
    expect(renderToStaticMarkup(<RiskWarnings />)).toBe("")
  })
  it("leads the risk panel", () => {
    vi.mocked(useLive).mockReturnValue(liveState(status, null, "open"))
    const html = renderToStaticMarkup(<QueryClientProvider client={new QueryClient()}><RiskPanel risk={{ ...risk, warnings: [ratchet] }} /></QueryClientProvider>)
    expect(html.indexOf("Floor rises tonight")).toBeLessThan(html.indexOf("Daily loss"))
  })
})
