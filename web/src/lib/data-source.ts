import { useSyncExternalStore } from "react"

/** Where the terminal reads its market and account from: the live feed, or the replay running beside it. */
export type DataSource = "live" | "replay"

/** In memory only: a reload always opens on the live feed. */
export function createSourceStore() {
  let source: DataSource = "live"
  const listeners = new Set<() => void>()
  return {
    get: () => source,
    set(next: DataSource) {
      if (next === source) return
      source = next
      listeners.forEach((listener) => listener())
    },
    subscribe(listener: () => void) { listeners.add(listener); return () => { listeners.delete(listener) } },
  }
}
export const dataSource = createSourceStore()
export function useDataSource() {
  return useSyncExternalStore(dataSource.subscribe, dataSource.get, () => "live" as const)
}
