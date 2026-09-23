import { useSyncExternalStore } from "react"

/** Live media query; false where matchMedia is unavailable (server rendering, tests). */
export function useMediaQuery(query: string): boolean {
  return useSyncExternalStore(
    (callback) => {
      if (typeof window === "undefined" || !window.matchMedia) return () => {}
      const list = window.matchMedia(query)
      list.addEventListener("change", callback)
      return () => list.removeEventListener("change", callback)
    },
    () => typeof window !== "undefined" && !!window.matchMedia && window.matchMedia(query).matches,
    () => false,
  )
}
