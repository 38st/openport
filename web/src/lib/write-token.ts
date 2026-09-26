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

type TokenStore = ReturnType<typeof createTokenStore>
/** A link carrying the write token after "#token=", which openportd prints when it
 * keeps its token in a file (as the Docker image does), saves it for this tab. The
 * fragment never reaches the server, and the address bar is left without it. */
export function adoptLinkedToken(location: Pick<Location, "hash" | "pathname" | "search">,
                                 history: Pick<History, "replaceState">, store: TokenStore = writeToken) {
  const match = /^#token=([^&#]*)$/.exec(location.hash)
  if (!match) return false
  let token = ""
  try { token = decodeURIComponent(match[1] ?? "").trim() } catch { /* a malformed link saves nothing */ }
  history.replaceState(null, "", location.pathname + location.search)
  if (!token) return false
  store.set(token)
  return true
}
export function useWriteToken() {
  return useSyncExternalStore(writeToken.subscribe, writeToken.get, () => "")
}
