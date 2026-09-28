import { QueryClient, QueryClientProvider } from "@tanstack/react-query"
import { renderToStaticMarkup } from "react-dom/server"
import { afterEach, expect, it, vi } from "vitest"
import { App } from "./App"
import { liveState, useLive } from "./api/live"
import { useWriteToken } from "./lib/write-token"
import { status } from "./test/trading-fixtures"

vi.mock("./api/live", async (original) => ({ ...await original<typeof import("./api/live")>(), useLive: vi.fn() }))
vi.mock("./lib/write-token", async (original) => ({ ...await original<typeof import("./lib/write-token")>(), useWriteToken: vi.fn() }))
vi.mock("./lib/route", async (original) => ({ ...await original<typeof import("./lib/route")>(), useRoute: () => [{ view: "orders", symbol: "SPX" }, vi.fn()] }))
vi.mock("./lib/theme", () => ({ useTheme: () => ({ theme: "dark", toggleTheme: vi.fn() }) }))
afterEach(() => vi.clearAllMocks())

it.each([
  ["token", "", 1], ["token", "held-token", 0], ["open", "", 0], ["disabled", "", 0],
] as const)("shows one watch-only notice for write=%s and token=%s", (write, token, count) => {
  const trading = { ...status.trading!, write }
  vi.mocked(useLive).mockReturnValue(liveState({ ...status, trading }, null, "open"))
  vi.mocked(useWriteToken).mockReturnValue(token)
  const client = new QueryClient({ defaultOptions: { queries: { retry: false } } })
  const html = renderToStaticMarkup(<QueryClientProvider client={client}><App /></QueryClientProvider>)
  expect(html.match(/Watch only\. Trading on this server needs its write token\./g) ?? []).toHaveLength(count)
  if (count) expect(html).toContain("Enter write token")
  if (write === "disabled") expect(html).toContain("Read only · trading writes disabled by server")
  client.clear()
})
