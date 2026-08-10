/**
 * @file Unit tests for the keyed promise combinators.
 *   Each `describe` block below is one of the six spec invariants the native
 *   implementation carries, plus the behavioral cases a caller would notice:
 *   key ordering, an empty dictionary, already-settled inputs,
 *   first-rejection-wins, both allSettled statuses, and a subclass `this`.
 *   These run against `test/reference/keyed-combinators.mts`, the executable
 *   twin of the C++ in `src/socketsecurity/promise/keyed_combinators.cc`. The
 *   native path only exists inside a built node-smol binary; the conformance
 *   gate (`test262:promise-keyed`) is what judges that one. This file is what
 *   keeps the ALGORITHM honest on every commit, which is where the invariants
 *   are easy to break and hard to notice.
 */

import { describe, expect, it } from 'vitest'

import {
  allKeyedReference,
  allSettledKeyedReference,
  enumerableOwnKeys,
  installKeyedCombinators,
} from '../reference/keyed-combinators.mts'
import type { KeyedCombinator } from '../reference/keyed-combinators.mts'

type Dictionary = Record<string, unknown>

/**
 * Every combinator here returns `unknown`, because a Promise-like host may hand
 * back anything. These two predicates are how a test narrows that safely: a
 * type assertion would claim the shape, a predicate checks it and throws when
 * the claim is wrong, which is itself a failing test rather than a silent pass.
 */
function isDictionary(value: unknown): value is Dictionary {
  return typeof value === 'object' && value !== null
}

function isKeyedCombinator(value: unknown): value is KeyedCombinator {
  return typeof value === 'function'
}

/**
 * A Promise-like host whose `resolve` can be swapped out. Instances are
 * thenable, so a combinator's returned capability promise is awaitable.
 */
class ExampleHost {
  readonly inner: Promise<unknown>

  constructor(
    executor: (
      resolve: (value: unknown) => void,
      reject: (reason: unknown) => void,
    ) => void,
  ) {
    this.inner = new Promise(executor)
  }

  // A Promise-like host has to be thenable; that is what makes the
  // combinator's returned capability promise awaitable in these tests.
  // oxlint-disable-next-line unicorn/no-thenable -- deliberate test host
  then(
    onFulfilled?: ((value: unknown) => unknown) | undefined,
    onRejected?: ((reason: unknown) => unknown) | undefined,
  ): Promise<unknown> {
    return this.inner.then(onFulfilled, onRejected)
  }

  static resolve(value: unknown): unknown {
    return Promise.resolve(value)
  }
}

function allKeyed(dictionary: unknown, host: unknown = Promise): unknown {
  return Reflect.apply(allKeyedReference, host, [dictionary])
}

function allSettledKeyed(
  dictionary: unknown,
  host: unknown = Promise,
): unknown {
  return Reflect.apply(allSettledKeyedReference, host, [dictionary])
}

async function settledKeys(result: unknown): Promise<Dictionary> {
  const settled = await result
  if (!isDictionary(settled)) {
    throw new TypeError(
      'expected the combinator to resolve with a dictionary result',
    )
  }
  return settled
}

describe('invariant: the result object has a null prototype', () => {
  it('resolves with an object that inherits nothing', async () => {
    const result = await settledKeys(allKeyed({ exampleKey: 1 }))
    expect(Object.getPrototypeOf(result)).toBe(null)
  })

  it('stores a __proto__ key as an ordinary data property', async () => {
    // On a normal object this assignment would invoke the inherited
    // __proto__ setter and silently reparent the object instead of storing
    // anything. A null prototype is what makes the write a plain data
    // property, which is the spec's CreateDataPropertyOrThrow.
    const result = await settledKeys(
      allKeyed({ ['__proto__']: 'example-value' }),
    )
    expect(Object.getPrototypeOf(result)).toBe(null)
    expect(Object.getOwnPropertyDescriptor(result, '__proto__')?.value).toBe(
      'example-value',
    )
  })
})

describe('invariant: remaining starts at 1 and drops after the key walk', () => {
  it('waits for every key even when all inputs are already settled', async () => {
    // With a 0-based counter the first already-settled key would reach 0 and
    // resolve the combinator with a one-key result. All three must land.
    const result = await settledKeys(
      allKeyed({
        firstKey: Promise.resolve('first-value'),
        secondKey: Promise.resolve('second-value'),
        thirdKey: Promise.resolve('third-value'),
      }),
    )
    expect(Object.keys(result)).toEqual(['firstKey', 'secondKey', 'thirdKey'])
    expect(result['thirdKey']).toBe('third-value')
  })

  it('mixes already-settled and pending inputs without settling early', async () => {
    let releasePending: ((value: unknown) => void) | undefined
    const pending = new Promise(resolve => {
      releasePending = resolve
    })
    let combinatorSettled = false
    const combined = allKeyed({
      settledKey: Promise.resolve('settled-value'),
      pendingKey: pending,
    })
    void Promise.resolve(combined).then(() => {
      combinatorSettled = true
    })

    // Drain the microtask queue the already-settled key runs on.
    await Promise.resolve()
    await Promise.resolve()
    expect(combinatorSettled).toBe(false)

    releasePending!('pending-value')
    const result = await settledKeys(combined)
    expect(result).toEqual({
      settledKey: 'settled-value',
      pendingKey: 'pending-value',
    })
  })
})

describe('invariant: own enumerable keys only', () => {
  it('skips a non-enumerable own key', async () => {
    const dictionary: Dictionary = { visibleKey: 'visible-value' }
    Object.defineProperty(dictionary, 'hiddenKey', {
      configurable: true,
      enumerable: false,
      value: 'hidden-value',
      writable: true,
    })
    const result = await settledKeys(allKeyed(dictionary))
    expect(Object.keys(result)).toEqual(['visibleKey'])
  })

  it('skips an inherited enumerable key', async () => {
    const parent = { inheritedKey: 'inherited-value' }
    const dictionary: Dictionary = Object.create(parent)
    dictionary['ownKey'] = 'own-value'
    const result = await settledKeys(allKeyed(dictionary))
    expect(Object.keys(result)).toEqual(['ownKey'])
  })

  it('skips a symbol key', async () => {
    const exampleSymbol = Symbol('example-symbol')
    const dictionary: Dictionary = { stringKey: 'string-value' }
    Reflect.set(dictionary, exampleSymbol, 'symbol-value')
    const result = await settledKeys(allKeyed(dictionary))
    expect(Object.keys(result)).toEqual(['stringKey'])
    expect(Object.getOwnPropertySymbols(result)).toEqual([])
  })

  it('exposes the key walk on its own so the filter is testable directly', () => {
    const dictionary: Dictionary = { secondKey: 2 }
    Object.defineProperty(dictionary, 'nonEnumerableKey', {
      enumerable: false,
      value: 3,
    })
    expect(enumerableOwnKeys(dictionary)).toEqual(['secondKey'])
  })
})

describe('invariant: allSettledKeyed shares alreadyCalled between handlers', () => {
  it('counts a thenable that calls both callbacks exactly once', async () => {
    // A thenable reached through a host whose `resolve` hands back a raw
    // object: `Promise.resolve` would have normalized the double call away,
    // so the host is how the misbehaving thenable stays observable.
    class DoubleSettlingHost extends ExampleHost {
      static override resolve(value: unknown): unknown {
        if (value === 'double-settling') {
          return {
            // The misbehaving thenable under test.
            // oxlint-disable-next-line unicorn/no-thenable -- fixture
            then(
              onFulfilled: (v: unknown) => void,
              onRejected: (r: unknown) => void,
            ) {
              onFulfilled('first-call-wins')
              onRejected(new Error('second call must not count'))
            },
          }
        }
        return Promise.resolve(value)
      }
    }

    let releasePending: ((value: unknown) => void) | undefined
    const pending = new Promise(resolve => {
      releasePending = resolve
    })

    const combined = allSettledKeyed(
      { doubleKey: 'double-settling', pendingKey: pending },
      DoubleSettlingHost,
    )

    let combinatorSettled = false
    void Promise.resolve(combined).then(() => {
      combinatorSettled = true
    })
    await Promise.resolve()
    await Promise.resolve()

    // A per-handler flag would have decremented twice here, hitting zero and
    // resolving with `pendingKey` missing entirely.
    expect(combinatorSettled).toBe(false)

    releasePending!('pending-value')
    const result = await settledKeys(combined)
    expect(Object.keys(result)).toEqual(['doubleKey', 'pendingKey'])
    expect(result['doubleKey']).toEqual({
      status: 'fulfilled',
      value: 'first-call-wins',
    })
  })
})

describe('invariant: name and length are observable', () => {
  it('installs each combinator with name and length 1', () => {
    const target: Dictionary = {}
    installKeyedCombinators(target)
    const allKeyedFn = target['allKeyed']
    const allSettledFn = target['allSettledKeyed']
    if (!isKeyedCombinator(allKeyedFn) || !isKeyedCombinator(allSettledFn)) {
      throw new TypeError('expected both combinators to install as functions')
    }
    expect(allKeyedFn.name).toBe('allKeyed')
    expect(allKeyedFn.length).toBe(1)
    expect(allSettledFn.name).toBe('allSettledKeyed')
    expect(allSettledFn.length).toBe(1)
  })

  it('installs with the builtin-method descriptor', () => {
    const target: Dictionary = {}
    installKeyedCombinators(target)
    const descriptor = Object.getOwnPropertyDescriptor(target, 'allKeyed')
    expect(descriptor?.enumerable).toBe(false)
    expect(descriptor?.writable).toBe(true)
    expect(descriptor?.configurable).toBe(true)
  })

  it('leaves an existing implementation alone', () => {
    // A future V8 that ships the proposal wins: its version is the one
    // test262 and the ecosystem track.
    const existing = () => 'native-wins'
    const target: Dictionary = { allKeyed: existing }
    installKeyedCombinators(target)
    expect(target['allKeyed']).toBe(existing)
  })
})

describe('invariant: TypeError paths', () => {
  it('rejects, not throws, on a non-object argument', async () => {
    const combined = allKeyed(42)
    expect(combined).toBeInstanceOf(Promise)
    await expect(combined).rejects.toThrow(TypeError)
  })

  it('rejects, not throws, when `resolve` is not callable', async () => {
    class UncallableResolveHost extends ExampleHost {
      // A non-callable `resolve` is the point of the test, so the field is
      // typed as unknown rather than narrowed to a callable it is not.
      static override resolve: unknown = 'not-a-function'
    }
    const combined = allKeyed({ exampleKey: 1 }, UncallableResolveHost)
    await expect(Promise.resolve(combined)).rejects.toThrow(TypeError)
  })

  it('throws synchronously on a non-constructor `this`', () => {
    // NewPromiseCapability is step 2 and the spec spells it with `?`, so
    // there is no capability yet whose reject could carry the error. This is
    // the one TypeError of the three that does NOT become a rejection.
    expect(() => allKeyed({ exampleKey: 1 }, () => undefined)).toThrow(
      TypeError,
    )
    expect(() => allKeyed({ exampleKey: 1 }, 'not-a-constructor')).toThrow(
      TypeError,
    )
  })
})

describe('behavior', () => {
  it('preserves [[OwnPropertyKeys]] order: integer indices first, then insertion', async () => {
    const dictionary: Dictionary = {
      '2': Promise.resolve('two'),
      zebraKey: Promise.resolve('zebra'),
      '1': Promise.resolve('one'),
      appleKey: Promise.resolve('apple'),
    }
    const result = await settledKeys(allKeyed(dictionary))
    expect(Object.keys(result)).toEqual(['1', '2', 'zebraKey', 'appleKey'])
  })

  it('resolves an empty dictionary with an empty null-prototype object', async () => {
    const result = await settledKeys(allKeyed({}))
    expect(Object.keys(result)).toEqual([])
    expect(Object.getPrototypeOf(result)).toBe(null)
  })

  it('lets the first rejection win for allKeyed', async () => {
    const firstError = new Error('first-rejection')
    const secondError = new Error('second-rejection')
    await expect(
      allKeyed({
        firstKey: Promise.reject(firstError),
        secondKey: Promise.reject(secondError),
      }),
    ).rejects.toBe(firstError)
  })

  it('reports both statuses for allSettledKeyed', async () => {
    const exampleError = new Error('example-rejection')
    const result = await settledKeys(
      allSettledKeyed({
        okKey: Promise.resolve('ok-value'),
        failKey: Promise.reject(exampleError),
      }),
    )
    expect(result).toEqual({
      okKey: { status: 'fulfilled', value: 'ok-value' },
      failKey: { status: 'rejected', reason: exampleError },
    })
  })

  it('never rejects for allSettledKeyed even when every input rejects', async () => {
    const result = await settledKeys(
      allSettledKeyed({
        firstKey: Promise.reject(new Error('first-rejection')),
        secondKey: Promise.reject(new Error('second-rejection')),
      }),
    )
    expect(Object.keys(result)).toEqual(['firstKey', 'secondKey'])
  })

  it('constructs the returned promise from a subclass `this`', async () => {
    class TrackingPromise extends Promise<unknown> {}
    const combined = allKeyed(
      { exampleKey: Promise.resolve(1) },
      TrackingPromise,
    )
    expect(combined).toBeInstanceOf(TrackingPromise)
    expect(await settledKeys(combined)).toEqual({ exampleKey: 1 })
  })

  it('routes every key through the subclass `resolve`', async () => {
    const seen: unknown[] = []
    class ObservingHost extends ExampleHost {
      static override resolve(value: unknown): unknown {
        seen.push(value)
        return Promise.resolve(value)
      }
    }
    const combined = allKeyed(
      { firstKey: 'first-value', secondKey: 'second-value' },
      ObservingHost,
    )
    await Promise.resolve(combined)
    expect(seen).toEqual(['first-value', 'second-value'])
  })
})
