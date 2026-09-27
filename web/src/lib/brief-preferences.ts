import { useSyncExternalStore } from "react"

const key = "openport.brief-levels"
const event = "openport:brief-levels"
let fallback = false
export function loadBriefLevels() {
  try { return localStorage.getItem(key) === "true" } catch { return fallback }
}
export function saveBriefLevels(value: boolean) {
  fallback = value
  try { localStorage.setItem(key, String(value)) } catch { /* Private browsing can deny storage. */ }
  window.dispatchEvent(new Event(event))
}
function subscribe(callback: () => void) {
  window.addEventListener(event, callback)
  window.addEventListener("storage", callback)
  return () => { window.removeEventListener(event, callback); window.removeEventListener("storage", callback) }
}
export const useBriefLevels = () => useSyncExternalStore(subscribe, loadBriefLevels, loadBriefLevels)
