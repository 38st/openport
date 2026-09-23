import type { Expiry, Summary } from "../api/types"

const europeanIndices = new Set(["SPX", "SPXW", "NDX", "NDXP", "RUT", "RUTW", "VIX", "VIXW", "XSP"])

export function americanApproximation(symbol: string, summaryFlag: Summary["american_approximation"], style: Expiry["style"], deamericanized?: Expiry["deamericanized"]): boolean {
  if (deamericanized === true) return false
  // The selected expiry is more specific than the underlying-wide summary.
  if (style != null) return style === "american"
  return summaryFlag ?? !europeanIndices.has(symbol)
}

export function rateSourceHint(expiry: Expiry): string {
  switch (expiry.rate_source) {
    case "parity": return "Fitted from put-call parity"
    case "term": return "Borrowed from this underlying's longer expiries"
    case "curve": return `From the ${expiry.rate_curve_symbol ?? "European index"} parity curve: American-style parity is distorted by early exercise`
    case "assumed": return "Assumed flat rate: add SPX to get a market-implied curve"
    default: return expiry.rate_fitted ? "Fitted from put-call parity" : "Borrowed from this underlying's longer expiries"
  }
}
