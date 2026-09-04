/**
 * @file Path helpers for tui-infra.
 *   Single source of truth (1 path, 1 reference) for where tui-infra's
 *   C++ source lives. Consumers (currently node-smol-builder's additions
 *   copy step) import these instead of hardcoding paths.
 */

import path from 'node:path'
import { fileURLToPath } from 'node:url'

const __dirname = path.dirname(fileURLToPath(import.meta.url))

/**
 * Package root: packages/tui-infra/
 */
export const PACKAGE_ROOT = path.resolve(__dirname, '..')

/**
 * C++ source root: packages/tui-infra/src/socketsecurity/tui/
 */
export const TUI_SRC_DIR = path.join(
  PACKAGE_ROOT,
  'src',
  'socketsecurity',
  'tui',
)

/**
 * Public include root: packages/tui-infra/include/tui/
 */
export const TUI_INCLUDE_DIR = path.join(PACKAGE_ROOT, 'include', 'tui')

/**
 * Include search root the `#include "tui/…"` lines resolve against:
 * packages/tui-infra/include/
 */
export const TUI_INCLUDE_SEARCH_DIR = path.join(PACKAGE_ROOT, 'include')

/**
 * LibFuzzer harness root: packages/tui-infra/fuzz/fuzz_targets/
 */
export const TUI_FUZZ_TARGETS_DIR = path.join(
  PACKAGE_ROOT,
  'fuzz',
  'fuzz_targets',
)

/**
 * Isolated output root for the sanitized fuzz build:
 * packages/tui-infra/fuzz/build/ (gitignored).
 *
 * Sanitized and un-sanitized objects must never share an output dir — mixing
 * them yields a binary whose sanitizer runtime sees half the program, so the
 * fuzz build gets a tree of its own.
 */
export const TUI_FUZZ_BUILD_DIR = path.join(PACKAGE_ROOT, 'fuzz', 'build')

/**
 * Committed seed corpus root: packages/tui-infra/fuzz/corpus/
 */
export const TUI_FUZZ_CORPUS_DIR = path.join(PACKAGE_ROOT, 'fuzz', 'corpus')

/**
 * Upstream OpenTUI submodule root (read-only parity reference).
 */
export const UPSTREAM_OPENTUI_DIR = path.join(
  PACKAGE_ROOT,
  'upstream',
  'opentui',
)

/**
 * Upstream Yoga submodule root (read-only parity reference).
 */
export const UPSTREAM_YOGA_DIR = path.join(PACKAGE_ROOT, 'upstream', 'yoga')
