import { useQuery, useQueryClient } from "@tanstack/react-query"
import { createContext, useContext, useEffect, useMemo, useState, type ReactNode } from "react"
import { api } from "./client"
import { connectLive, type Connection } from "./connection"
import type { Status, Tick, UnderlyingSnapshot, UnderlyingStatus } from "./types"

export type { Connection } from "./connection"

interface Live {
  trading: Status["trading"]
  accountScope: number
  status: Status | undefined
  tick: Tick | null
  connection: Connection
  market: Status["market"]
  underlyings: (UnderlyingSnapshot & Partial<Pick<UnderlyingStatus, "expiries" | "options">>)[]
  /** Version of an underlying's data: queries include it in their key and refetch when it moves. */
  version: (symbol: string) => number
}

const LiveContext = createContext<Live | null>(null)

export function liveState(status: Status | undefined, tick: Tick | null, connection: Connection, accountScope = 0): Live {
  const connectedTick = connection === "open" ? tick : null
  return {
    // REST capability is required: old servers must never expose trading UI.
    trading: status?.trading ? (connectedTick?.trading === undefined ? status.trading : connectedTick.trading) : undefined,
    accountScope,
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
  const [connection, setConnection] = useState<Connection>("connecting")
  const [accountScope, setAccountScope] = useState(0)

  useEffect(() => {
    const scheme = window.location.protocol === "https:" ? "wss" : "ws"
    return connectLive(`${scheme}://${window.location.host}/ws`, queryClient, setTick, (next) => {
      setConnection(next)
      setAccountScope((scope) => scope + 1)
    })
  }, [queryClient])

  const status = useQuery({
    queryKey: ["status"],
    queryFn: ({ signal }) => api.status(signal),
    refetchInterval: connection === "open" ? 10_000 : 2_000,
  })

  const value = useMemo<Live>(
    () => liveState(status.data, tick, connection, accountScope),
    [status.data, tick, connection, accountScope],
  )
  return <LiveContext value={value}>{children}</LiveContext>
}

export function useLive(): Live {
  const live = useContext(LiveContext)
  if (!live) throw new Error("useLive outside LiveProvider")
  return live
}
