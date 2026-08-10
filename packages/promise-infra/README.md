# promise-infra

Native C++ implementation of `Promise.allKeyed` and `Promise.allSettledKeyed`
from [`tc39/proposal-await-dictionary`](https://github.com/tc39/proposal-await-dictionary),
compiled into node-smol as the `smol_promise` binding.

Both combinators are the dictionary-shaped siblings of `Promise.all` and
`Promise.allSettled`: they take an object whose values are promises and settle
with an object carrying the same keys.

```js
const { user, posts } = await Promise.allKeyed({
  user: fetchUser(),
  posts: fetchPosts(),
})
```

## Why native

Both are spec-observable builtins. Their `name`, `length`, the property
descriptor they are installed under, the order in which they touch `this`,
`Promise.resolve`, and each `then`, and which of their TypeErrors throw versus
reject are all things test262 asserts on. A builtin is the surface those
assertions describe, so that is the layer that owns them.

The install is feature-detected: `polyfills/promise-keyed.js` only defines a
combinator when the property is absent, so a future V8 that ships the proposal
keeps its own implementation.

## The six invariants

Each one is a spec detail a naive port drops, and each is checked twice - by
`scripts/check-lockstep.mts` (the mechanism is still in the C++) and by
`test/unit/keyed-combinators.test.mts` (the behavior is still correct):

| Invariant                           | Mechanism                                                                                                                                                                                                                                                             |
| ----------------------------------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Result object has a null prototype  | `NullProtoObject`, so no inherited setter can intercept a write, `__proto__` included. That is what makes a plain write `CreateDataPropertyOrThrow`                                                                                                                   |
| `remaining` starts at 1             | seeded before the key walk, decremented once after it, so a dictionary of already-settled promises cannot settle the combinator early                                                                                                                                 |
| Own enumerable keys only            | `PropertyFilter::ONLY_ENUMERABLE`, so an inherited or non-enumerable key is skipped the way the spec's `[[OwnPropertyKeys]]` walk skips it                                                                                                                            |
| `alreadyCalled` is shared           | one per-key record reaches BOTH `allSettledKeyed` handlers, so a thenable that calls both counts exactly once                                                                                                                                                         |
| `name` and `length` are explicit    | `Function::New`'s length argument plus `SetName`. An anonymous function would read as `''`                                                                                                                                                                            |
| TypeErrors reject rather than throw | a non-object argument and a non-callable `resolve` settle the returned promise. A non-constructor `this` is the exception: `NewPromiseCapability` is step 2, so there is no capability yet whose reject could carry the error, and the spec spells that step with `?` |

## Layout

- `src/socketsecurity/promise/` for the C++ implementation. Copied into
  node-smol's `additions/source-patched/src/socketsecurity/promise/` at build
  time by `prepare-external-sources.mts`, then compiled by node-smol's gyp
  pipeline, same path as `primordial_binding.cc`.
- `lib/paths.mts` for path helpers, exported via the workspace `exports` field.
  The test262 corpus paths are inherited from `local-temporal-infra`, which is
  where the shared submodule is mounted.
- `scripts/check-lockstep.mts` is the static gate: invariant anchors plus
  binding-registration completeness across the four wiring patches.
- `test/reference/` holds the algorithm in TypeScript, used as the unit tests'
  oracle. A full node-smol build takes hours; this keeps the invariants
  measurable on every commit.
- `test/scripts/` holds the test262 conformance runner for the keyed subset.
- `test262-config/test262.allowlist` holds known failures, in its own file
  rather than inline.

## Bootstrap wiring

Five files outside this package carry the binding, and
`scripts/check-lockstep.mts` fails loud when any of them stops doing so:

| File                                                                              | Role                                                                        |
| --------------------------------------------------------------------------------- | --------------------------------------------------------------------------- |
| `patches/source-patched/002-polyfills.patch`                                      | requires the installer from `lib/internal/bootstrap/node.js`                |
| `patches/source-patched/004-node-gyp-smol-sources.patch`                          | lists the `.cc` in node.gyp's smol sources                                  |
| `patches/source-patched/017-smol-builtin-bindings.patch`                          | `V(smol_promise)` in `NODE_BUILTIN_BINDINGS`                                |
| `patches/source-patched/019-smol-external-refs.patch`                             | `V(smol_promise)` in `EXTERNAL_REFERENCE_BINDING_LIST`, so mksnapshot links |
| `additions/source-patched/lib/internal/socketsecurity/polyfills/promise-keyed.js` | the feature-detected installer                                              |

## Testing

```sh
pnpm test packages/promise-infra
pnpm --filter promise-infra run check:lockstep
pnpm --filter promise-infra run test262:promise-keyed
```

The conformance runner needs a built node-smol binary and the sparse test262
checkout. Without the corpus on disk it reports the situation and exits 0
rather than claiming a pass over zero tests; the `verify =` field on the
submodule entry in `.gitmodules` is what makes the checked-out case get
exercised.

## Naming

Per the `*-infra` convention: source-only utility packages, no release
pipeline. Matches `temporal-infra` (the C++ Temporal port) and `bin-infra`.
