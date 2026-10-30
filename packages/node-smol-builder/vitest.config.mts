/**
 * Extends shared vitest config.
 * Excludes build directories which contain Node.js test fixtures.
 */
import { defineConfig, mergeConfig } from 'vitest/config'

import baseConfig from '../../.config/repo/vitest.config.mts'

// Vitest CLI auto-discovers config via default import.
// oxlint-disable-next-line socket/no-default-export -- vitest config contract
export default mergeConfig(
  baseConfig,
  defineConfig({
    test: {
      // globals (bare describe/it/expect, used by most of this package's test
      // files) is provided by the repo base config (.config/repo/vitest.config.mts).
      exclude: [
        '**/build/**',
        '**/dist/**',
        '**/node_modules/**',
        '**/upstream/**',
      ],
      // Integration tests share temp dirs and the ~/.socket/_dlx/ cache.
      // Run files sequentially to prevent race conditions.
      fileParallelism: false,
      setupFiles: ['./test/helpers/primordials-shim.mts'],
    },
  }),
)
