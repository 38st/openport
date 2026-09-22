import { useQuery } from "@tanstack/react-query"
import { createContext, useContext, useEffect, useMemo, useState, type ReactNode } from "react"
import { api } from "./client"
import type { Status, Tick } from "./types"

export type Connection = "connecting" | "open" | "closed"

interface Live {
  status: Status | undefined
  tick: Tick | null
  connection: Connection
  /** Version of an underlying's data: queries include it in their key and refetch when it moves. */
  version: (symbol: string) => number
}

const LiveContext = createContext<Live | null>(null)

/// Keeps one WebSocket to openportd open (reconnecting with backoff) and exposes the
/// latest tick. The REST status is fetched once and then only while the socket is down.
export function LiveProvider({ children }: { children: ReactNode }) {
  const [tick, setTick] = useState<Tick | null>(null)
  const [connection, setConnection] = useState<Connection>("connecting")

  useEffect(() => {
    let socket: WebSocket | null = null
    let retry = 0
    let timer: ReturnType<typeof setTimeout> | undefined
    let disposed = false

    const connect = () => {
      const scheme = window.location.protocol === "https:" ? "wss" : "ws"
      socket = new WebSocket(`${scheme}://${window.location.host}/ws`)
      socket.onopen = () => {
        retry = 0
        setConnection("open")
      }
      socket.onmessage = (event: MessageEvent<string>) => {
        const message = JSON.parse(event.data) as Tick
        if (message.type === "tick") setTick(message)
      }
      socket.onclose = () => {
        setConnection("closed")
        if (!disposed) timer = setTimeout(connect, Math.min(10_000, 500 * 2 ** retry++))
      }
    }
    connect()
    return () => {
      disposed = true
      clearTimeout(timer)
      socket?.close()
    }
  }, [])

  const status = useQuery({
    queryKey: ["status"],
    queryFn: api.status,
    refetchInterval: connection === "open" ? 10_000 : 2_000,
  })

  const value = useMemo<Live>(
    () => ({
      status: status.data,
      tick,
      connection,
      version: (symbol) =>
        tick?.underlyings.find((u) => u.symbol === symbol)?.version ??
        status.data?.underlyings.find((u) => u.symbol === symbol)?.version ??
        0,
    }),
    [status.data, tick, connection],
  )
  return <LiveContext value={value}>{children}</LiveContext>
}

export function useLive(): Live {
  const live = useContext(LiveContext)
  if (!live) throw new Error("useLive outside LiveProvider")
  return live
}
