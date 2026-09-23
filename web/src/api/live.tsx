import { useQuery, useQueryClient } from "@tanstack/react-query"
import { createContext, useContext, useEffect, useMemo, useState, type ReactNode } from "react"
import { api } from "./client"
import { connectLive, type Connection } from "./connection"
import type { Status, Tick } from "./types"

export type { Connection } from "./connection"

interface Live {
  status: Status | undefined
  tick: Tick | null
  connection: Connection
  /** Version of an underlying's data: queries include it in their key and refetch when it moves. */
  version: (symbol: string) => number
}

const LiveContext = createContext<Live | null>(null)

export function liveState(status: Status | undefined, tick: Tick | null, connection: Connection): Live {
  const connectedTick = connection === "open" ? tick : null
  return {
    status,
    tick: connectedTick,
    connection,
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

  useEffect(() => {
    const scheme = window.location.protocol === "https:" ? "wss" : "ws"
    return connectLive(`${scheme}://${window.location.host}/ws`, queryClient, setTick, setConnection)
  }, [queryClient])

  const status = useQuery({
    queryKey: ["status"],
    queryFn: ({ signal }) => api.status(signal),
    refetchInterval: connection === "open" ? 10_000 : 2_000,
  })

  const value = useMemo<Live>(
    () => liveState(status.data, tick, connection),
    [status.data, tick, connection],
  )
  return <LiveContext value={value}>{children}</LiveContext>
}

export function useLive(): Live {
  const live = useContext(LiveContext)
  if (!live) throw new Error("useLive outside LiveProvider")
  return live
}
