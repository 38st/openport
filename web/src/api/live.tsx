import { useQuery, useQueryClient } from "@tanstack/react-query"
import { createContext, useCallback, useContext, useEffect, useMemo, useRef, useState, type ReactNode } from "react"
import { api } from "./client"
import { connectLive, type Connection } from "./connection"
import { activeAccount, MAIN_ACCOUNT, useActiveAccount } from "../lib/active-account"
import { dataSource, useDataSource, type DataSource } from "../lib/data-source"
import type { AccountBrief, ReplayState, Status, Tick, UnderlyingSnapshot, UnderlyingStatus } from "./types"

export type { Connection } from "./connection"

interface Live {
  /** The active account's trading status. */
  trading: Status["trading"]
  accountScope: number
  /** Every paper account, and the one the terminal acts on. */
  accounts: AccountBrief[]
  account: string
  switchAccount: (id: string) => void
  /** Whether the terminal shows the live feed or the replay running beside it, and that replay. */
  source: DataSource
  replay: ReplayState | null
  switchSource: (source: DataSource) => void
  status: Status | undefined
  tick: Tick | null
  connection: Connection
  market: Status["market"]
  underlyings: (UnderlyingSnapshot & Partial<Pick<UnderlyingStatus, "expiries" | "options">>)[]
  /** Version of an underlying's data: queries include it in their key and refetch when it moves. */
  version: (symbol: string) => number
}

const LiveContext = createContext<Live | null>(null)

export function liveState(status: Status | undefined, tick: Tick | null, connection: Connection, accountScope = 0,
  account = MAIN_ACCOUNT, switchAccount: (id: string) => void = () => {},
  source: DataSource = "live", replay: ReplayState | null = null, switchSource: (source: DataSource) => void = () => {}): Live {
  const connectedTick = connection === "open" ? tick : null
  const accounts = connectedTick?.accounts ?? status?.accounts ?? []
  const main = connectedTick?.trading === undefined ? status?.trading : connectedTick.trading
  // Another account's status comes from the account list; older servers have only the main one.
  const trading = account === MAIN_ACCOUNT ? main : accounts.find((a) => a.id === account)?.trading
  return {
    // REST capability is required: old servers must never expose trading UI.
    trading: status?.trading ? trading : undefined,
    accountScope,
    accounts,
    account,
    switchAccount,
    source,
    replay,
    switchSource,
    status,
    tick: connectedTick,
    connection,
    market: connectedTick?.market === undefined ? status?.market : connectedTick.market,
    underlyings: connectedTick
      ? connectedTick.underlyings.map((u) => ({ ...status?.underlyings.find((previous) => previous.symbol === u.symbol), ...u }))
      : status?.underlyings ?? [],
    version: (symbol) => connectedTick?.underlyings.find((u) => u.symbol === symbol)?.version
      ?? status?.underlyings.find((u) => u.symbol === symbol)?.version ?? 0,
  }
}

/// Keeps one WebSocket to openportd open (reconnecting with backoff) and exposes the
/// latest connected tick. REST status polls more often while the socket is down.
export function LiveProvider({ children }: { children: ReactNode }) {
  const queryClient = useQueryClient()
  const [tick, setTick] = useState<Tick | null>(null)
  const [replayTick, setReplayTick] = useState<Tick | null>(null)
  const replaySeen = useRef(0)
  const [connection, setConnection] = useState<Connection>("connecting")
  const [accountScope, setAccountScope] = useState(0)
  const account = useActiveAccount()
  const source = useDataSource()

  useEffect(() => {
    const scheme = window.location.protocol === "https:" ? "wss" : "ws"
    return connectLive(`${scheme}://${window.location.host}/ws`, queryClient, setTick, (next) => {
      setConnection(next)
      setAccountScope((scope) => scope + 1)
    }, (next) => {
      if (next) replaySeen.current = Date.now()
      setReplayTick(next)
    })
  }, [queryClient])

  const status = useQuery({
    queryKey: ["status", source],
    queryFn: ({ signal }) => api.status(signal),
    refetchInterval: connection === "open" ? 10_000 : 2_000,
  })

  // A new account is a new scope: account-bound views remount and refetch.
  const switchAccount = useCallback((id: string) => {
    activeAccount.set(id)
    setAccountScope((scope) => scope + 1)
  }, [])
  // Everything cached came from the other source: drop it, and remount account views.
  const switchSource = useCallback((next: DataSource) => {
    if (dataSource.get() === next) return
    dataSource.set(next)
    queryClient.removeQueries()
    setAccountScope((scope) => scope + 1)
  }, [queryClient])
  // A replay that stopped, here or in another window, returns the terminal to the live feed.
  useEffect(() => {
    if (source !== "replay") return
    const timer = setInterval(() => { if (Date.now() - replaySeen.current > 5_000) switchSource("live") }, 1_000)
    return () => clearInterval(timer)
  }, [source, switchSource])
  // Fall back to the main account when the active one is gone (or the server keeps only one).
  const known = tick?.accounts ?? (source === "live" ? status.data?.accounts ?? (status.data ? [] : undefined) : undefined)
  useEffect(() => {
    if (source === "live" && known && account !== MAIN_ACCOUNT && !known.some((a) => a.id === account)) switchAccount(MAIN_ACCOUNT)
  }, [source, known, account, switchAccount])
  const value = useMemo<Live>(
    () => liveState(status.data, source === "replay" ? replayTick : tick, connection, accountScope,
      source === "replay" ? MAIN_ACCOUNT : account, switchAccount, source, replayTick?.replay ?? null, switchSource),
    [status.data, tick, replayTick, connection, accountScope, account, switchAccount, source, switchSource],
  )
  return <LiveContext value={value}>{children}</LiveContext>
}

export function useLive(): Live {
  const live = useContext(LiveContext)
  if (!live) throw new Error("useLive outside LiveProvider")
  return live
}
