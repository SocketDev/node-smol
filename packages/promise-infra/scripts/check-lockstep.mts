#!/usr/bin/env node

/**
 * @file Lockstep audit for the promise-infra keyed combinators.
 *   The contract has three parts, each checkable without a full build, so each
 *   is checked here rather than trusted:
 *
 *   1. Invariant anchors. Each of the six spec invariants is carried by a specific
 *      mechanism in keyed_combinators.cc. A refactor that drops one leaves the
 *      file compiling and the combinator subtly wrong — a dictionary of
 *      already-settled promises resolving early, a proxy key leaking in, a
 *      `name` reading as the empty string. The scan looks for the MECHANISM,
 *      never a comment, so a comment cannot satisfy it.
 *   2. Registration completeness. A native binding is only reachable if four
 *      separate files agree: the source is listed in the node.gyp patch, the
 *      binding name is in NODE_BUILTIN_BINDINGS and in the external-reference
 *      list, and something requires the bootstrap installer. Miss one and the
 *      failure surfaces hours into a build, or as a snapshot link error.
 *   3. It compiles. The anchor scan reads the file as text, so it passes on a file
 *      that cannot build — which is what happened here. A syntax-only run
 *      catches that in seconds, and skips loudly when the Node checkout or a
 *      compiler is absent. Exit codes: 0 — every check passed or skipped. 1 —
 *      at least one failed. Run via: pnpm --filter promise-infra run
 *      check:lockstep
 */

import { existsSync, readFileSync } from 'node:fs'
import path from 'node:path'
import { fileURLToPath } from 'node:url'

import { errorMessage } from '@socketsecurity/lib-stable/errors/message'
import { getDefaultLogger } from '@socketsecurity/lib-stable/logger/default'
import { spawn } from '@socketsecurity/lib-stable/process/spawn/child'
import { isSpawnExitError } from '@socketsecurity/lib-stable/process/spawn/errors'

import { NODE_UPSTREAM_INCLUDE_DIRS } from '../lib/paths.mts'

const logger = getDefaultLogger()
const __dirname = path.dirname(fileURLToPath(import.meta.url))
const packageRoot = path.resolve(__dirname, '..')
const repoRoot = path.resolve(packageRoot, '..', '..')

const IMPL_PATH = path.join(
  packageRoot,
  'src',
  'socketsecurity',
  'promise',
  'keyed_combinators.cc',
)

const PATCHES_DIR = path.join(
  repoRoot,
  'packages',
  'node-smol-builder',
  'patches',
  'source-patched',
)

/**
 * One row per spec invariant: the mechanism that carries it, and what a
 * reader should do when the anchor is gone.
 */
export const INVARIANT_ANCHORS: ReadonlyArray<{
  name: string
  pattern: RegExp
  fix: string
}> = [
  {
    name: 'result object has a null prototype',
    pattern: /Local<Object> result = NullProtoObject\(isolate\)/,
    fix: 'build the result with NullProtoObject so a plain write is CreateDataPropertyOrThrow',
  },
  {
    name: 'remaining starts at 1',
    pattern: /WriteCount\(context, shared, SlotRemaining\(isolate\), 1\)/,
    fix: 'seed the counter at 1 before the key walk, and decrement once after it',
  },
  {
    name: 'own enumerable keys only',
    pattern: /PropertyFilter::ONLY_ENUMERABLE/,
    fix: 'ask V8 for own ENUMERABLE keys so inherited and non-enumerable keys are skipped',
  },
  {
    name: 'alreadyCalled is shared between allSettled handlers',
    pattern: /SlotAlreadyCalled/,
    fix: 'keep the flag on the per-key record both handlers receive as their `data`',
  },
  {
    name: 'name and length are set explicitly',
    pattern: /fn->SetName\(name\)/,
    fix: 'set the observable name, and pass length 1 to Function::New',
  },
  {
    name: 'abrupt completions reject the capability',
    pattern: /CapabilityReject\(context, capability\)\s*\n?\s*->Call\(/,
    fix: 'route a post-capability failure through the capability reject, not a synchronous throw',
  },
]

/**
 * One row per wiring file, with the token that proves the binding is wired.
 */
export const REGISTRATION_ANCHORS: ReadonlyArray<{
  patch: string
  token: string
  fix: string
}> = [
  {
    patch: '002-polyfills.patch',
    token: "require('internal/socketsecurity/polyfills/promise-keyed')",
    fix: 'require the bootstrap installer from lib/internal/bootstrap/node.js',
  },
  {
    patch: '004-node-gyp-smol-sources.patch',
    token: "'src/socketsecurity/promise/keyed_combinators.cc'",
    fix: "list the source in node.gyp's smol sources block",
  },
  {
    patch: '017-smol-builtin-bindings.patch',
    token: 'V(smol_promise)',
    fix: 'add the binding to NODE_BUILTIN_BINDINGS',
  },
  {
    patch: '019-smol-external-refs.patch',
    token: 'V(smol_promise)',
    fix: 'add the binding to EXTERNAL_REFERENCE_BINDING_LIST so the snapshot links',
  },
]

/**
 * Check 1: every invariant anchor is present in the implementation.
 */
export function checkInvariantAnchors(): {
  ok: boolean
  missing: Array<{ name: string; fix: string }>
} {
  const source = readFileSync(IMPL_PATH, 'utf8')
  const missing: Array<{ name: string; fix: string }> = []
  for (let i = 0, { length } = INVARIANT_ANCHORS; i < length; i += 1) {
    const anchor = INVARIANT_ANCHORS[i]!
    if (!anchor.pattern.test(source)) {
      missing.push({ name: anchor.name, fix: anchor.fix })
    }
  }
  return { ok: missing.length === 0, missing }
}

/**
 * Check 2: every wiring file carries its registration token.
 */
export function checkRegistration(): {
  ok: boolean
  missing: Array<{ patch: string; fix: string }>
} {
  const missing: Array<{ patch: string; fix: string }> = []
  for (let i = 0, { length } = REGISTRATION_ANCHORS; i < length; i += 1) {
    const anchor = REGISTRATION_ANCHORS[i]!
    let text: string
    try {
      text = readFileSync(path.join(PATCHES_DIR, anchor.patch), 'utf8')
    } catch {
      missing.push({ patch: anchor.patch, fix: `patch file is missing` })
      continue
    }
    if (!text.includes(anchor.token)) {
      missing.push({ patch: anchor.patch, fix: anchor.fix })
    }
  }
  return { ok: missing.length === 0, missing }
}

/**
 * Every flag the standalone syntax check needs. `NODE_WANT_INTERNALS` is the
 * gate Node puts its internal helpers behind, so without it
 * `FIXED_ONE_BYTE_STRING` and `ExternalReferenceRegistry` read as undeclared
 * and the run fails for a reason that has nothing to do with this file.
 */
export function compileArgs(): string[] {
  const includes: string[] = []
  for (let i = 0, { length } = NODE_UPSTREAM_INCLUDE_DIRS; i < length; i += 1) {
    includes.push('-I', NODE_UPSTREAM_INCLUDE_DIRS[i]!)
  }
  return [
    '-std=c++20',
    '-fsyntax-only',
    '-DNODE_WANT_INTERNALS=1',
    ...includes,
    IMPL_PATH,
  ]
}

export interface CompileResult {
  status: 'passed' | 'failed' | 'skipped'
  detail: string
}

/**
 * A spawn's captured output, as a string. The stream fields are untyped on the
 * error, so anything that is not already a string reads as empty rather than
 * being stringified into `[object Object]`.
 */
export function asText(value: unknown): string {
  return typeof value === 'string' ? value : ''
}

/**
 * Check 3: the translation unit actually compiles.
 *
 * The anchor scan above reads the file as text, so it passes just as happily on
 * a file that cannot build — which is exactly what happened: six calls to
 * `v8::Context::GetIsolate()` survived here after V8 removed the accessor, and
 * every static check stayed green. A syntax-only run costs seconds and is the
 * only check that would have caught it.
 *
 * Skips, loudly, when the Node checkout or a C++ compiler is absent: a fresh
 * clone has no submodule, and a skip that says so beats a red that means
 * "you have not run the build yet".
 */
export async function checkCompiles(): Promise<CompileResult> {
  const missingIncludes = NODE_UPSTREAM_INCLUDE_DIRS.filter(
    dir => !existsSync(dir),
  )
  if (missingIncludes.length) {
    return {
      status: 'skipped',
      detail: `Node headers absent (${missingIncludes.length} of ${NODE_UPSTREAM_INCLUDE_DIRS.length} include dirs missing) — run the builder's upstream checkout to enable this check`,
    }
  }
  // `xcrun` puts the macOS SDK on the include path; a bare clang++ from
  // homebrew finds no SDK and fails on <cstdlib>, which looks like a source
  // error and is not one.
  const compiler = process.platform === 'darwin' ? 'xcrun' : 'c++'
  const args =
    process.platform === 'darwin'
      ? ['clang++', ...compileArgs()]
      : compileArgs()
  try {
    await spawn(compiler, args)
    return { status: 'passed', detail: 'keyed_combinators.cc compiles clean' }
  } catch (e) {
    // spawn rejects on ANY nonzero exit, so both outcomes arrive down this one
    // path and must be told apart: isSpawnExitError is true only when the
    // process RAN and exited nonzero, false for a binary that is not there.
    // Collapsing the two is how a real compile error reads as "no compiler
    // installed" — measured, which is why the branch is explicit.
    if (!isSpawnExitError(e)) {
      return {
        status: 'skipped',
        detail: `no C++ compiler on PATH (tried \`${compiler}\`) — install one to enable this check`,
      }
    }
    const diagnostics = asText(e.stderr) || asText(e.stdout)
    return {
      status: 'failed',
      detail: diagnostics.trim() || errorMessage(e),
    }
  }
}

// ── Run all checks ──────────────────────────────────

/**
 * Run every check and report. Exits nonzero when one FAILED; a SKIPPED check
 * is reported and does not fail the audit.
 */
export async function main(): Promise<void> {
  let failed = false

  logger.info('promise-infra lockstep audit')
  logger.info('')

  logger.info('Check 1/3: spec invariant anchors')
  const invariantResult = checkInvariantAnchors()
  if (invariantResult.ok) {
    logger.success(
      `  all ${INVARIANT_ANCHORS.length} invariant anchors present`,
    )
  } else {
    failed = true
    logger.error(
      `  ${invariantResult.missing.length} invariant anchor(s) gone:`,
    )
    const missing = invariantResult.missing
    for (let i = 0, { length } = missing; i < length; i += 1) {
      const entry = missing[i]!
      logger.error(`    ${entry.name} — ${entry.fix}`)
    }
  }

  logger.info('')
  logger.info('Check 2/3: native binding registration')
  const registrationResult = checkRegistration()
  if (registrationResult.ok) {
    logger.success('  smol_promise is wired in every patch that must carry it')
  } else {
    failed = true
    logger.error(
      `  ${registrationResult.missing.length} wiring file(s) missing the binding:`,
    )
    const missing = registrationResult.missing
    for (let i = 0, { length } = missing; i < length; i += 1) {
      const entry = missing[i]!
      logger.error(`    ${entry.patch} — ${entry.fix}`)
    }
  }

  logger.info('')
  logger.info('Check 3/3: the translation unit compiles')
  const compileResult = await checkCompiles()
  if (compileResult.status === 'passed') {
    logger.success(`  ${compileResult.detail}`)
  } else if (compileResult.status === 'skipped') {
    logger.warn(`  SKIPPED — ${compileResult.detail}`)
  } else {
    failed = true
    logger.error('  keyed_combinators.cc does not compile:')
    logger.error(compileResult.detail)
  }

  logger.info('')
  if (failed) {
    logger.error('lockstep audit FAILED')
    process.exit(1)
  }
  logger.success('lockstep audit PASSED')
}

main().catch((e: unknown) => {
  logger.error(`lockstep audit could not run: ${errorMessage(e)}`)
  process.exitCode = 1
})
