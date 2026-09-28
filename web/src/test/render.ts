import { act } from "react"
import { vi } from "vitest"

// These deadlines guard liveness, not rendering speed.
export const renderTimeout = 5 * 60_000

export async function waitForRender(check: () => void) {
  await vi.waitFor(async () => {
    await act(async () => {})
    check()
  }, { timeout: renderTimeout })
}
