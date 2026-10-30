/**
 * Extends shared vitest config.
 * Excludes build directory (contains large ONNX model files).
 */
import { defineConfig, mergeConfig } from 'vitest/config'

import baseConfig from '../../.config/repo/vitest.config.mts'

// Vitest CLI auto-discovers config via default import.
// oxlint-disable-next-line socket/no-default-export -- vitest config contract
export default mergeConfig(
  baseConfig,
  defineConfig({
    test: {
      exclude: ['**/build/**', '**/node_modules/**'],
    },
  }),
)
