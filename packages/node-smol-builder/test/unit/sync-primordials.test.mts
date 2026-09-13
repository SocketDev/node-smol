import { describe, expect, it } from 'vitest'

import { primordialImportsForUsage } from '../../scripts/vendor-fast-webstreams/sync-primordials.mts'

describe(primordialImportsForUsage, () => {
  it('selects only the Promise methods used by an ordinary module', () => {
    expect(
      primordialImportsForUsage({
        filename: 'readable-stream.js',
        usesNewPromise: false,
        usesPromiseAll: false,
        usesPromiseReject: true,
        usesPromiseResolve: true,
      }),
    ).toEqual({
      injectSettleAll: false,
      primordialImports: ['PromiseResolve', 'PromiseReject'],
    })
  })

  it('deduplicates SafePromise for the pipe-to settle helper', () => {
    expect(
      primordialImportsForUsage({
        filename: 'pipe-to.js',
        usesNewPromise: true,
        usesPromiseAll: true,
        usesPromiseReject: false,
        usesPromiseResolve: false,
      }),
    ).toEqual({
      injectSettleAll: true,
      primordialImports: ['SafePromise', 'PromisePrototypeThen'],
    })
  })
})
