import type { QueryClient } from "@tanstack/react-query"
import type { Tick } from "./types"
import { writeToken } from "../lib/write-token"

export type Connection = "connecting" | "open" | "closed"

/** Subscribe once per provider; return cleanup for unmount and StrictMode. */
export function connectLive(
  url: string,
  queryClient: QueryClient,
  onTick: (tick: Tick | null) => void,
  onConnection: (connection: Connection) => void,
  onReplayTick: (tick: Tick | null) => void = () => {},
) {
  let socket: WebSocket | null = null
  let retry = 0
  let timer: ReturnType<typeof setTimeout> | undefined
  let disposed = false

  const connect = () => {
    const token = writeToken.get()
    const protocols = token ? ["openport", `openport.token.${Array.from(new TextEncoder().encode(token), (byte) => byte.toString(16).padStart(2, "0")).join("")}`] : undefined
    const current = protocols ? new WebSocket(url, protocols) : new WebSocket(url)
    socket = current
    current.onopen = () => {
      if (disposed || socket !== current) return
      retry = 0
      onTick(null)
      onConnection("open")
      // A restarted server may reuse old version numbers. Invalidate every key,
      // including inactive ones, and cancel responses from the previous session.
      void queryClient.cancelQueries().then(() => {
        if (!disposed && socket === current) return queryClient.invalidateQueries()
      })
    }
    current.onmessage = (event: MessageEvent<string>) => {
      if (disposed || socket !== current) return
      const message = JSON.parse(event.data) as Tick
      if (message.type === "tick") onTick(message)
      else if (message.type === "replay_tick") onReplayTick(message)
    }
    current.onclose = () => {
      if (disposed || socket !== current) return
      onTick(null)
      onReplayTick(null)
      onConnection("closed")
      void queryClient.invalidateQueries({ queryKey: ["status"] })
      timer = setTimeout(connect, Math.min(10_000, 500 * 2 ** retry++))
    }
  }
  connect()
  return () => {
    disposed = true
    clearTimeout(timer)
    socket?.close()
  }
}
