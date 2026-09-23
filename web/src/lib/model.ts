import type { Expiry, Summary } from "../api/types"

const europeanIndices = new Set(["SPX", "SPXW", "NDX", "NDXP", "RUT", "RUTW", "VIX", "VIXW", "XSP"])

export function americanApproximation(symbol: string, summaryFlag: Summary["american_approximation"], style: Expiry["style"]): boolean {
  // The selected expiry is more specific than the underlying-wide summary.
  if (style != null) return style === "american"
  return summaryFlag ?? !europeanIndices.has(symbol)
}
