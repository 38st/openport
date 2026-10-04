import { defineConfig, mergeConfig } from "vitest/config"
import viteConfig from "./vite.config"
import { testTimeout } from "./src/test/timeout"

export default mergeConfig(viteConfig, defineConfig({
  test: { testTimeout, hookTimeout: testTimeout },
}))
