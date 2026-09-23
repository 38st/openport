import type { FeedState, ProviderInfo } from "../api/types"
import { isNum } from "./format"

export function providerLabel(provider: ProviderInfo, state?: FeedState | null): string {
  if (provider.simulated) return "simulated prices"
  const delayed = isNum(provider.delay_seconds) && provider.delay_seconds > 0
    ? `${provider.delay_seconds / 60}-min delay` : "delayed"
  if (provider.realtime_plan_dependent) {
    if (state === "live") return "real-time"
    if (state === "delayed") return delayed
    return "timing unconfirmed"
  }
  return provider.realtime ? "real-time" : delayed
}
