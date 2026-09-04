'use strict'

/*
 * Socket Security: Install Promise.allKeyed / Promise.allSettledKeyed
 *
 * The two keyed promise combinators from
 * https://github.com/tc39/proposal-await-dictionary. Both are implemented
 * natively in `src/socketsecurity/promise/keyed_combinators.cc` and exposed
 * through the `smol_promise` internal binding; this file is only the
 * installer that puts them on the `Promise` constructor during bootstrap.
 *
 * WHY THIS EXISTS:
 * - The proposal is not in V8 yet, so `Promise.allKeyed` is absent
 * - Awaiting a dictionary is the shape most callers actually want, and
 *   `Promise.all` over `Object.entries` loses the key/value association
 *
 * WHEN IT ACTIVATES:
 * - Only when the property is absent. A future V8 that ships the proposal
 *   natively keeps its own implementation: V8's version is the one test262
 *   and the wider ecosystem track, so it wins on any collision
 *
 * WHAT IT INSTALLS:
 * - `Promise.allKeyed` and `Promise.allSettledKeyed`, each with the
 *   standard builtin-method descriptor (writable, non-enumerable,
 *   configurable). `name` and `length` come from the native functions
 *   themselves
 */

// Use primordials for protection against prototype pollution.
const { ObjectDefineProperty } = primordials

const { allKeyed, allSettledKeyed } = internalBinding('smol_promise')

// Standard attributes for a builtin method: assignable and deletable, but
// skipped by for-in and Object.keys.
function installCombinator(name, fn) {
  if (Promise[name] !== undefined) {
    return
  }
  ObjectDefineProperty(Promise, name, {
    __proto__: null,
    configurable: true,
    enumerable: false,
    value: fn,
    writable: true,
  })
}

installCombinator('allKeyed', allKeyed)
installCombinator('allSettledKeyed', allSettledKeyed)
