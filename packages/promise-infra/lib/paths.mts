/**
 * @file Path helpers for promise-infra.
 *   Single source of truth (1 path, 1 reference) for where promise-infra's
 *   C++ source lives. Consumers (currently node-smol-builder's additions
 *   copy step) import these instead of hardcoding paths.
 *   The test262 corpus itself is NOT owned here. One submodule serves both
 *   conformance runners, and temporal-infra is where it is mounted, so its
 *   paths.mts stays the owner and this file inherits them.
 */

import path from 'node:path'
import { fileURLToPath } from 'node:url'

import { UPSTREAM_PATH } from 'local-node-smol-builder/scripts/paths'
import {
  TEST262_HARNESS_DIR,
  TEST262_ROOT,
  TEST262_TEST_DIR,
} from 'local-temporal-infra/lib/paths'

const __dirname = path.dirname(fileURLToPath(import.meta.url))

export { TEST262_HARNESS_DIR, TEST262_ROOT, TEST262_TEST_DIR }

/**
 * Include dirs a translation unit in this package needs to compile against the
 * Node checkout, inherited from the builder's UPSTREAM_PATH rather than rebuilt
 * here. Node's own headers reach V8 and libuv by these three roots, so a
 * standalone syntax check needs all three to resolve `node_internals.h`.
 */
export const NODE_UPSTREAM_INCLUDE_DIRS: readonly string[] = [
  path.join(UPSTREAM_PATH, 'src'),
  path.join(UPSTREAM_PATH, 'deps', 'v8', 'include'),
  path.join(UPSTREAM_PATH, 'deps', 'uv', 'include'),
]

/**
 * Package root: packages/promise-infra/
 */
export const PACKAGE_ROOT = path.resolve(__dirname, '..')

/**
 * C++ source root: packages/promise-infra/src/socketsecurity/promise/
 */
export const PROMISE_SRC_DIR = path.join(
  PACKAGE_ROOT,
  'src',
  'socketsecurity',
  'promise',
)

/**
 * Promise API tests: test/built-ins/Promise/
 *
 * Added to the corpus submodule's sparse-checkout list alongside the
 * Temporal subsets; the keyed combinators live under
 * `allKeyed/` and `allSettledKeyed/` inside it.
 */
export const TEST262_PROMISE_BUILTINS_DIR = path.join(
  TEST262_TEST_DIR,
  'built-ins',
  'Promise',
)

/**
 * The known-failure allowlist for the keyed-combinator subset. Its own file
 * rather than an inline list, per the conformance-runner layout.
 */
export const TEST262_ALLOWLIST_PATH = path.join(
  PACKAGE_ROOT,
  'test262-config',
  'test262.allowlist',
)
