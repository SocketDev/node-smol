import { defineConfig } from 'vitest/config'

// oxlint-disable-next-line socket/no-default-export -- vitest config contract
export default defineConfig({
  root: import.meta.dirname,
  test: {
    exclude: [
      '**/build/**',
      '**/dist/**',
      '**/node_modules/**',
      '**/upstream/**',
    ],
    fileParallelism: false,
    include: ['test/e2e/e2e.test.mts'],
    setupFiles: ['./test/helpers/primordials-shim.mts'],
  },
})
