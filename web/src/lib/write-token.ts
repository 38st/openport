import { useSyncExternalStore } from "react"

const key = "openport.write-token"
/** Memory remains usable when storage is denied (including access to the getter). */
export function createTokenStore(storage: () => Pick<Storage, "getItem" | "setItem" | "removeItem">) {
  let memory: string | undefined
  const listeners = new Set<() => void>()
  return {
    get() {
      if (memory !== undefined) return memory
      try { memory = storage().getItem(key) ?? "" } catch { memory = "" }
      return memory
    },
    set(token: string) {
      memory = token.trim()
      let persisted = true
      try {
        if (memory) storage().setItem(key, memory)
        else storage().removeItem(key)
      } catch { persisted = false }
      listeners.forEach((listener) => listener())
      return persisted
    },
    subscribe(listener: () => void) { listeners.add(listener); return () => { listeners.delete(listener) } },
  }
}
export const writeToken = createTokenStore(() => window.sessionStorage)
export function useWriteToken() {
  return useSyncExternalStore(writeToken.subscribe, writeToken.get, () => "")
}
