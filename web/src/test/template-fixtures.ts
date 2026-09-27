import type { Chain } from "../api/types"
import { black76, normCdf } from "../lib/strategy"
import { chain, expiry, quote } from "./trading-fixtures"

/** Synthetic chains for deterministic template tests; these are not market observations. */
export const templateChain: Chain = {
  ...chain, spot: 100, expiry: { ...expiry, forward: 100, days: 73, atm_iv: .2 },
  strikes: Array.from({ length: 13 }, (_, i) => {
    const strike = 70 + i * 5
    const option = (type: "call" | "put") => {
      const mid = black76(type, 100, strike, .2, .2, .99)
      return { ...quote, symbol: `SPXW  261016${type === "call" ? "C" : "P"}${String(strike * 1000).padStart(8, "0")}`,
        mid, bid: Math.max(0, mid - .01), ask: mid + .01, delta: normCdf((100 - strike) / 10) - (type === "put" ? 1 : 0) }
    }
    return { strike, iv: .2, gex: null, vex: null, call: option("call"), put: option("put") }
  }),
}
export const farTemplateChain: Chain = {
  ...templateChain, expiry: { ...templateChain.expiry, id: "2026-11-20PM", expiry: "2026-11-20", expiry_time: "2026-11-20T21:00:00Z", days: 108 },
  strikes: templateChain.strikes.map((r) => ({ ...r,
    call: { ...r.call!, symbol: r.call!.symbol!.replace("261016", "261120") },
    put: { ...r.put!, symbol: r.put!.symbol!.replace("261016", "261120") },
  })),
}
