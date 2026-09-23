import { QueryClient, QueryClientProvider } from "@tanstack/react-query"
import { renderToStaticMarkup } from "react-dom/server"
import { describe, expect, it, vi } from "vitest"
import type { Surface, SviFit } from "../api/types"
import { SviTable } from "../components/SviTable"
import { SmileView } from "./SmileView"

vi.mock("../api/live", () => ({ useLive: () => ({ version: () => 1 }) }))
const fit: SviFit = { a: -.041, b: .1331, rho: .306, m: .3586, sigma: .4153, rmse_vol_points: .002,
  points: 60, status: "ok", reason: null, fit_ms: 1, butterfly_min_g: -.02, butterfly_k: 1.1, butterfly_ok: false }
const surface: Surface = { symbol: "SPX", spot: 5000, as_of: null, version: 1,
  calendar_violations: [{ earlier: "2027-01-15PM", later: "2027-02-15PM", k: .1, vol_points: .4 }],
  expiries: [{ id: "2027-01-15PM", expiry: "2027-01-15", days: 114, forward: 5000, atm_iv: .2,
    svi: fit, points: [-.1, .1].map((k) => ({ strike: 5000 * Math.exp(k), k, iv: .2, bid_iv: .19, ask_iv: .21 })) },
  { id: "2027-02-15PM", expiry: "2027-02-15", days: 145, forward: 5000, atm_iv: .2, svi: { ...fit, butterfly_ok: true }, points: [] },
  { id: "2027-03-15PM", expiry: "2027-03-15", days: 175, forward: 5000, atm_iv: null, svi: null,
    svi_status: "too_few_points", svi_reason: "need at least five distinct two-sided OTM IV points", svi_points: 3, points: [] }] }

describe("SVI view", () => {
  it("shows parameters, units, explicit failures and both arbitrage badges", () => {
    const html = renderToStaticMarkup(<SviTable expiries={surface.expiries} violations={surface.calendar_violations!} />)
    expect(html).toContain("Butterfly violation")
    expect(html).toContain("calendar: 0.4 vp at k=0.10 vs Feb 15")
    expect(html).toContain("calendar: 0.4 vp at k=0.10 vs Jan 15")
    expect(html).toContain("Too few points")
    expect(html).toContain("need at least five distinct two-sided OTM IV points")
    expect(html).toContain("-0.04100")
    expect(html).toContain("0.0020")
    expect(html).toContain("RMSE in vol points")
    expect(html).not.toContain("Grid checks pass")
    expect(html).not.toContain("NaN")
  })
  it("renders a keyboard accessible market/SVI/both control and scoped grid pass label", () => {
    const client = new QueryClient({ defaultOptions: { queries: { staleTime: Infinity, gcTime: Infinity } } })
    client.setQueryData(["surface", "SPX", 6, .1, 1], { ...surface, calendar_violations: [] })
    const html = renderToStaticMarkup(<QueryClientProvider client={client}><SmileView symbol="SPX" /></QueryClientProvider>)
    client.clear()
    expect(html).toContain('aria-label="Smile display"')
    expect(html).toContain('aria-checked="true" tabindex="0" class=')
    expect(html).toContain(">Market</button>")
    expect(html).toContain(">SVI</button>")
    expect(html).toContain(">Both</button>")
    expect(html).toContain("Grid checks pass")
    expect(html).toContain("finite grid")
  })
})
