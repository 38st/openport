import { useEffect, useLayoutEffect, useState } from "react"

export type Theme = "light" | "dark"
export const themeStorageKey = "openport-theme"

export function resolveTheme(preference: string | null, systemDark: boolean): Theme {
  return preference === "light" || preference === "dark" ? preference : systemDark ? "dark" : "light"
}

export function useTheme() {
  const [preference, setPreference] = useState<string | null>(() => {
    try { return localStorage.getItem(themeStorageKey) } catch { return null }
  })
  const [systemDark, setSystemDark] = useState(() => window.matchMedia("(prefers-color-scheme: dark)").matches)
  const theme = resolveTheme(preference, systemDark)

  useLayoutEffect(() => {
    document.documentElement.classList.toggle("dark", theme === "dark")
    document.documentElement.style.colorScheme = theme
  }, [theme])

  useEffect(() => {
    const system = window.matchMedia("(prefers-color-scheme: dark)")
    const onSystem = () => setSystemDark(system.matches)
    const onStorage = (event: StorageEvent) => {
      if (event.key === themeStorageKey || event.key === null) setPreference(event.newValue)
    }
    onSystem()
    system.addEventListener("change", onSystem)
    window.addEventListener("storage", onStorage)
    return () => {
      system.removeEventListener("change", onSystem)
      window.removeEventListener("storage", onStorage)
    }
  }, [])

  const toggleTheme = () => {
    const next = theme === "dark" ? "light" : "dark"
    setPreference(next)
    try { localStorage.setItem(themeStorageKey, next) } catch { /* Keep the in-memory choice. */ }
  }
  return { theme, toggleTheme }
}
