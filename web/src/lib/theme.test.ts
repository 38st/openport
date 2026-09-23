import { describe, expect, it } from "vitest"
import html from "../../index.html?raw"
import { resolveTheme, themeStorageKey } from "./theme"

describe("theme resolution and first paint", () => {
  const bootstrap = html.match(/<script>([\s\S]*?)<\/script>/)?.[1] ?? ""
  const runBootstrap = new Function("localStorage", "window", "document", bootstrap)

  it.each([null, "light", "dark", "invalid", ""])("resolves preference %s consistently before and after React loads", (preference) => {
    for (const systemDark of [true, false]) {
      const expected = preference === "light" || preference === "dark" ? preference : systemDark ? "dark" : "light"
      expect(resolveTheme(preference, systemDark)).toBe(expected)
      const root = { classList: { toggle: (_name: string, enabled: boolean) => { dark = enabled } }, style: { colorScheme: "" } }
      let dark = false
      runBootstrap(
        { getItem: (key: string) => { expect(key).toBe(themeStorageKey); return preference } },
        { matchMedia: () => ({ matches: systemDark }) },
        { documentElement: root },
      )
      expect(dark).toBe(expected === "dark")
      expect(root.style.colorScheme).toBe(expected)
    }
  })

  it("runs before the app and survives blocked localStorage", () => {
    expect(html).not.toContain('<html lang="en" class="dark">')
    expect(html.indexOf(bootstrap)).toBeLessThan(html.indexOf('<div id="root">'))
    for (const systemDark of [true, false]) {
      let dark = false
      runBootstrap(
        { getItem: () => { throw new Error("blocked") } },
        { matchMedia: () => ({ matches: systemDark }) },
        { documentElement: { classList: { toggle: (_: string, enabled: boolean) => { dark = enabled } }, style: {} } },
      )
      expect(dark).toBe(systemDark)
    }
  })
})

