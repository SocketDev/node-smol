import { readFileSync } from 'node:fs'
import { compileFunction } from 'node:vm'

import { describe, expect, it } from 'vitest'

const bootstrapSources = [
  {
    name: 'primordials',
    path: '../../additions/source-patched/lib/internal/per_context/primordials.js',
    parameters: ['primordials'],
  },
  {
    name: 'promise-keyed polyfill',
    path: '../../additions/source-patched/lib/internal/socketsecurity/polyfills/promise-keyed.js',
    parameters: [
      'exports',
      'require',
      'module',
      'process',
      'internalBinding',
      'primordials',
    ],
  },
  {
    name: 'locale-compare polyfill',
    path: '../../additions/source-patched/lib/internal/socketsecurity/polyfills/locale-compare.js',
    parameters: [
      'exports',
      'require',
      'module',
      'process',
      'internalBinding',
      'primordials',
    ],
  },
]

describe('Node bootstrap source syntax', () => {
  it.each(bootstrapSources)(
    'compiles $name as a function body',
    ({ name, path, parameters }) => {
      const source = readFileSync(new URL(path, import.meta.url), 'utf8')

      expect(() =>
        compileFunction(source, parameters, { filename: name }),
      ).not.toThrow()
    },
  )
})
