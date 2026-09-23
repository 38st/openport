import { queryOptions, useQuery, useQueryClient } from "@tanstack/react-query"
import { useEffect, useRef } from "react"
import { api } from "./client"
import { useLive } from "./live"

export function tradingQueries(scope: number, version: string | undefined, enabled: boolean) {
  // The contract has one account per server and no account ID. A connection
  // epoch isolates potentially different accounts, even when versions repeat.
  const common = { enabled, placeholderData: <T,>(previous: T | undefined, query: { queryKey: readonly unknown[] } | undefined) => sameAccountPlaceholder(previous, query?.queryKey, scope), gcTime: 60_000 }
  return {
    portfolio: queryOptions({ ...common, queryKey: ["trading", scope, "portfolio", version], queryFn: ({ signal }) => api.portfolio(signal) }),
    orders: queryOptions({ ...common, queryKey: ["trading", scope, "orders", version], queryFn: ({ signal }) => api.orders("open", signal) }),
    fills: queryOptions({ ...common, queryKey: ["trading", scope, "fills", version], queryFn: ({ signal }) => api.fills(signal) }),
    risk: queryOptions({ ...common, queryKey: ["trading", scope, "risk", version], queryFn: ({ signal }) => api.risk(signal) }),
  }
}
export function sameAccountPlaceholder<T>(data: T | undefined, key: readonly unknown[] | undefined, scope: number) {
  return key?.[0] === "trading" && key[1] === scope ? data : undefined
}
export function useTradingQueries() {
  const { trading, accountScope } = useLive()
  const options = tradingQueries(accountScope, trading?.account_version, trading?.enabled === true)
  return { portfolio: useQuery(options.portfolio), orders: useQuery(options.orders), fills: useQuery(options.fills), risk: useQuery(options.risk) }
}
export function useRefreshTrading() {
  const client = useQueryClient()
  return () => Promise.all([
    client.invalidateQueries({ queryKey: ["trading"] }),
    client.invalidateQueries({ queryKey: ["status"] }),
  ])
}
/** Prevent a late mutation response from being rendered in another connection. */
export function useTradingSession() {
  const { accountScope } = useLive()
  const current = useRef<number | null>(accountScope)
  current.current = accountScope
  useEffect(() => { current.current = accountScope; return () => { current.current = null } }, [accountScope])
  return () => current.current === accountScope
}
