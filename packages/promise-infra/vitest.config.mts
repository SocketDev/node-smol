/**
 * Extends shared vitest config — promise-infra's unit tests run the keyed
 * combinators against an in-process reference with no binary spawning, so the
 * 30s timeout is comfortably over-budget. The integration test spawns the
 * conformance runner and carries its own longer per-test timeout.
 */
import { defineConfig, mergeConfig } from 'vitest/config'

import baseConfig from '../../.config/repo/vitest.config.mts'

// Vitest CLI auto-discovers config via default import.
// oxlint-disable-next-line socket/no-default-export -- vitest config contract
export default mergeConfig(
  baseConfig,
  defineConfig({
    test: {
      include: ['test/**/*.test.mts'],
      testTimeout: 30_000,
    },
  }),
)
