/**
 * @file Executable spec oracle for the keyed promise combinators.
 *   The shipping implementation is native C++
 *   (src/socketsecurity/promise/keyed_combinators.cc) and only runs inside a
 *   built node-smol binary, which takes hours to produce. This module is the
 *   same algorithm in TypeScript so the unit tests can assert the six spec
 *   invariants on every run, against a stock Node, in milliseconds.
 *   Read it as the C++ file's twin: same step order, same slots, same
 *   decisions. Two differences are deliberate and neither is observable for
 *   an ordinary object dictionary:
 *
 *   1. The C++ asks V8 for own enumerable keys in one call, so a Proxy dictionary
 *      sees one `ownKeys` trap. Here the descriptor check is spelled out, which
 *      costs one `getOwnPropertyDescriptor` trap per key.
 *   2. `new C(executor)` stands in for NewPromiseCapability's constructor check; a
 *      non-constructor `C` throws the same TypeError either way.
 */

type Settler = (value: unknown) => void

/**
 * Which combinator is running. Mirrors the C++ `Combinator` enum, and reads
 * at the call site where a bare boolean would not.
 */
export type Combinator = 'allKeyed' | 'allSettledKeyed'

type Capability = {
  promise: unknown
  resolve: Settler
  reject: Settler
}

/**
 * A Promise-like host: the `new C(executor)` NewPromiseCapability calls, plus
 * the `resolve` GetPromiseResolve reads. `resolve` stays `unknown` because a
 * host carrying a non-callable one is one of the TypeError paths under test, so
 * a narrower type here would hide the case rather than describe it.
 */
type PromiseHost = {
  new (executor: (resolve: Settler, reject: Settler) => void): unknown
  resolve?: unknown | undefined
}

/**
 * What `installKeyedCombinators` writes onto a host: one dictionary argument,
 * and a `this` the combinator reads as the host constructor.
 */
export type KeyedCombinator = (this: unknown, dictionary: unknown) => unknown

type SharedState = {
  remaining: number
  result: Record<string, unknown>
  resolve: Settler
}

/**
 * A value a property can be read off. Every value this module handles arrives
 * as `unknown`, and this narrowing is what makes a keyed read legal without a
 * type assertion.
 */
function isObjectLike(value: unknown): value is Record<string, unknown> {
  if (value === null) {
    return false
  }
  return typeof value === 'function' || typeof value === 'object'
}

/**
 * The callable half of IsConstructor(C). A callable that is not a constructor
 * passes here and throws from `new C(executor)` instead, which is where the
 * spec's step 2 throws it.
 */
function isPromiseHost(value: unknown): value is PromiseHost {
  return typeof value === 'function'
}

/**
 * NewPromiseCapability(C). `new C(executor)` throws for a callable that is not
 * a constructor, and `runKeyed` lets that propagate rather than rejecting:
 * the spec spells this step with `?`, so the capability whose reject would
 * carry the error does not exist yet.
 */
export function newPromiseCapability(constructor: PromiseHost): Capability {
  let resolve: Settler | undefined
  let reject: Settler | undefined
  const promise = new constructor((res, rej) => {
    if (resolve !== undefined || reject !== undefined) {
      throw new TypeError(
        'Promise capability executor was already invoked: a constructor may call its executor only once',
      )
    }
    resolve = res
    reject = rej
  })
  if (typeof resolve !== 'function' || typeof reject !== 'function') {
    throw new TypeError(
      "Promise capability was not populated: the constructor's executor must be called with two callable arguments",
    )
  }
  return { promise, resolve, reject }
}

/**
 * Own ENUMERABLE string keys, in [[OwnPropertyKeys]] order. The descriptor
 * check is what skips a non-enumerable key; the string filter is what skips
 * a symbol key; walking own keys only is what skips an inherited one.
 */
export function enumerableOwnKeys(dictionary: object): string[] {
  const keys: string[] = []
  const ownKeys = Reflect.ownKeys(dictionary)
  for (let i = 0, { length } = ownKeys; i < length; i += 1) {
    const key = ownKeys[i]!
    if (typeof key !== 'string') {
      continue
    }
    const descriptor = Object.getOwnPropertyDescriptor(dictionary, key)
    if (descriptor?.enumerable === true) {
      keys.push(key)
    }
  }
  return keys
}

/**
 * Everything after the capability exists. Throwing from here is how the
 * caller learns to reject rather than propagate.
 */
function performKeyed(
  constructor: PromiseHost,
  capability: Capability,
  input: unknown,
  kind: Combinator,
): void {
  // GetPromiseResolve(C) — read once, before the key walk, so no getter can
  // observe the iteration.
  const promiseResolve = constructor.resolve
  if (typeof promiseResolve !== 'function') {
    throw new TypeError(
      'Promise keyed combinator needs a callable `resolve`: `this.resolve` is not a function',
    )
  }
  if (!isObjectLike(input)) {
    throw new TypeError(
      'Promise keyed combinator needs an object: the argument is a dictionary of promises, not a primitive or an iterable',
    )
  }
  const dictionary = input

  // Plain assignment onto a null-prototype object IS
  // CreateDataPropertyOrThrow: with no prototype there is no inherited
  // setter to intercept the write, `__proto__` included.
  const result: Record<string, unknown> = Object.create(null)

  // The counter starts at 1, and the loop's own decrement lands only after
  // the walk ends. A dictionary of already-settled promises would otherwise
  // reach 0 on its first key and resolve the combinator before the rest of
  // the keys had been read.
  const shared: SharedState = {
    remaining: 1,
    result,
    resolve: capability.resolve,
  }

  const settledMode = kind === 'allSettledKeyed'
  const keys = enumerableOwnKeys(dictionary)
  for (let i = 0, { length } = keys; i < length; i += 1) {
    const key = keys[i]!
    const value = dictionary[key]
    const nextPromise: unknown = Reflect.apply(promiseResolve, constructor, [
      value,
    ])

    // One record per key, and for allSettledKeyed BOTH handlers close over
    // it. A thenable that calls its fulfill and its reject callback
    // therefore counts exactly once.
    const element = { alreadyCalled: false }
    const settleElement = (entry: unknown): void => {
      if (element.alreadyCalled) {
        return
      }
      element.alreadyCalled = true
      result[key] = entry
      shared.remaining -= 1
      if (shared.remaining === 0) {
        shared.resolve(result)
      }
    }

    const onFulfilled: Settler = settledMode
      ? fulfilledValue =>
          settleElement({ status: 'fulfilled', value: fulfilledValue })
      : fulfilledValue => settleElement(fulfilledValue)
    // allKeyed hands the capability's own reject to `then`, so the first
    // rejection wins with nothing else to coordinate.
    const onRejected: Settler = settledMode
      ? reason => settleElement({ status: 'rejected', reason })
      : capability.reject

    shared.remaining += 1

    // Invoke(nextPromise, "then", …) — a read plus a call, so a subclass
    // that overrides `then` is honored.
    const then = isObjectLike(nextPromise) ? nextPromise['then'] : undefined
    if (typeof then !== 'function') {
      throw new TypeError(
        'Promise keyed combinator needs a callable `then` on each resolved value',
      )
    }
    Reflect.apply(then, nextPromise, [onFulfilled, onRejected])
  }

  shared.remaining -= 1
  if (shared.remaining === 0) {
    shared.resolve(result)
  }
}

function runKeyed(
  constructor: unknown,
  input: unknown,
  kind: Combinator,
): unknown {
  // The non-callable half of the same step 2: a primitive `this` has no
  // `new C(executor)` to call at all. It throws outside the try below for the
  // reason NewPromiseCapability does, there is no capability yet.
  if (!isPromiseHost(constructor)) {
    throw new TypeError(
      'Promise keyed combinator needs a constructor `this`: the receiver is not callable, so it has no `new C(executor)`',
    )
  }
  const capability = newPromiseCapability(constructor)
  // IfAbruptRejectPromise: past the capability, every failure settles the
  // returned promise instead of propagating, so a caller that wrote
  // `await Promise.allKeyed(x)` has one failure channel rather than two.
  try {
    performKeyed(constructor, capability, input, kind)
  } catch (e) {
    capability.reject(e)
  }
  return capability.promise
}

export function allKeyedReference(this: unknown, dictionary: unknown): unknown {
  return runKeyed(this, dictionary, 'allKeyed')
}

export function allSettledKeyedReference(
  this: unknown,
  dictionary: unknown,
): unknown {
  return runKeyed(this, dictionary, 'allSettledKeyed')
}

/**
 * Install both combinators onto a Promise-like constructor with the
 * descriptor and the observable `name` / `length` a builtin carries. The
 * bootstrap installer
 * (lib/internal/socketsecurity/polyfills/promise-keyed.js) does the same
 * thing with the native functions, and the same absent-only check.
 */
export function installKeyedCombinators(target: Record<string, unknown>): void {
  const entries: Array<[string, KeyedCombinator]> = [
    ['allKeyed', allKeyedReference],
    ['allSettledKeyed', allSettledKeyedReference],
  ]
  for (let i = 0, { length } = entries; i < length; i += 1) {
    const [name, fn] = entries[i]!
    if (target[name] !== undefined) {
      continue
    }
    Object.defineProperty(fn, 'name', {
      configurable: true,
      enumerable: false,
      value: name,
      writable: false,
    })
    Object.defineProperty(fn, 'length', {
      configurable: true,
      enumerable: false,
      value: 1,
      writable: false,
    })
    Object.defineProperty(target, name, {
      configurable: true,
      enumerable: false,
      value: fn,
      writable: true,
    })
  }
}
