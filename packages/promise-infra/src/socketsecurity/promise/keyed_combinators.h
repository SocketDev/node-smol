// node:smol-promise V8 binding — Promise.allKeyed / Promise.allSettledKeyed.
//
// ─── What is this module? ──────────────────────────────────────────────
//
// The two keyed promise combinators from
// https://github.com/tc39/proposal-await-dictionary, implemented as native
// C++ builtins. They are the dictionary-shaped siblings of `Promise.all`
// and `Promise.allSettled`: instead of an iterable of promises they take a
// plain object whose values are promises, and they settle with an object
// carrying the same keys.
//
//   const { user, posts } = await Promise.allKeyed({
//     user: fetchUser(),
//     posts: fetchPosts(),
//   })
//
// ─── Why native instead of a JS shim? ──────────────────────────────────
//
// Both combinators are spec-observable builtins: their `name`, `length`,
// the property descriptor they are installed under, the ORDER in which
// they touch `this`, `Promise.resolve`, and each `then`, and which of
// their TypeErrors throw synchronously versus reject the returned promise
// are all things test262 asserts on. A builtin is the surface those
// assertions describe, so this is the layer that owns them.
//
// ─── How the state is threaded ─────────────────────────────────────────
//
// A JS builtin like this needs closures: each per-key handler has to reach
// a shared counter, the shared result object, and the capability's resolve
// function. C++ callbacks have no closures, so the state travels as the
// callback's `data` value — an object whose slots are V8 *private*
// symbols. Private symbols are invisible to JS (no `Object.keys`, no
// `Reflect.ownKeys`, no proxy trap), so the state cannot be observed or
// tampered with by the dictionary being awaited.
//
// Two records, mirroring the spec's two closure scopes:
//
//   - the combinator-wide record — remaining count, result object,
//     capability resolve function (`kSlotRemaining` / `kSlotResult` /
//     `kSlotResolve`).
//   - the per-key record — the key, a pointer back to the
//     combinator-wide record, and the shared `alreadyCalled` flag
//     (`kSlotKey` / `kSlotShared` / `kSlotAlreadyCalled`).
//
// `allSettledKeyed` hands the SAME per-key record to both its fulfill and
// its reject handler, which is how a thenable that calls both callbacks
// still counts exactly once.

#ifndef SRC_SOCKETSECURITY_PROMISE_KEYED_COMBINATORS_H_
#define SRC_SOCKETSECURITY_PROMISE_KEYED_COMBINATORS_H_

#include "node_external_reference.h"
#include "v8.h"

namespace node {
namespace socketsecurity {
namespace promise {

// `Promise.allKeyed(dictionary)` — settles with an object of values, or
// rejects with the first rejection.
void AllKeyed(const v8::FunctionCallbackInfo<v8::Value>& args);

// `Promise.allSettledKeyed(dictionary)` — always fulfills, with an object
// of `{ status, value }` / `{ status, reason }` records.
void AllSettledKeyed(const v8::FunctionCallbackInfo<v8::Value>& args);

void Initialize(v8::Local<v8::Object> target,
                v8::Local<v8::Value> unused,
                v8::Local<v8::Context> context,
                void* priv);

void RegisterExternalReferences(ExternalReferenceRegistry* registry);

}  // namespace promise
}  // namespace socketsecurity
}  // namespace node

#endif  // SRC_SOCKETSECURITY_PROMISE_KEYED_COMBINATORS_H_
