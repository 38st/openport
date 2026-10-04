import { act } from "react"
import { vi } from "vitest"
import { testTimeout } from "./timeout"

// These deadlines guard liveness, not rendering speed.
export const renderTimeout = testTimeout

export async function waitForRender(check: () => void) {
  await vi.waitFor(async () => {
    await act(async () => {})
    check()
  }, { timeout: renderTimeout })
}
