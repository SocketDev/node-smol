import { fileURLToPath } from 'node:url'

import { defineConfig } from 'vitest/config'

// oxlint-disable-next-line socket/no-default-export -- vitest config contract
export default defineConfig({
  root: fileURLToPath(new URL('../..', import.meta.url)),
  test: {
    exclude: [
      '**/build/**',
      '**/dist/**',
      '**/node_modules/**',
      '**/upstream/**',
    ],
    fileParallelism: false,
    include: ['packages/node-smol-builder/test/e2e/e2e.test.mts'],
    setupFiles: [
      './packages/node-smol-builder/test/helpers/primordials-shim.mts',
    ],
  },
})
