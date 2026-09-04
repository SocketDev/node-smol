#!/usr/bin/env node
/**
 * @file Test262 keyed-promise-combinator subset runner — CLI + main. Drives
 *   the `test/built-ins/Promise/allKeyed/` and
 *   `test/built-ins/Promise/allSettledKeyed/` trees of the shared test262
 *   corpus through the built node-smol binary, classifies each result against
 *   an allowlist of known-failures, and exits non-zero on regression OR stale
 *   allowlist entry. Same 4-tier layout and the same `--include` filter
 *   vocabulary as
 *   `packages/temporal-infra/test/scripts/test262-temporal-runner.mts`, and it
 *   reuses that runner's tiers rather than copying them:
 *
 *   - types.mts — result types (TestCase, Test, Summary, ...)
 *   - parser.mts — frontmatter parser
 *   - classifier.mts — interpret + emptyBuckets
 *   - harness.mts — loadHarness, composeScript, walkTests
 *   - report.mts — report Two things are this subset's own, and they live here.
 *     First, the corpus filter: `built-ins/Promise/` holds every Promise test,
 *     and only the two keyed directories belong to this gate. Second, async
 *     verdicts: nearly every keyed test is `flags: [async]` and reports through
 *     `doneprintHandle.js` on stdout rather than by exiting non-zero, so "did
 *     it throw" is not the question to ask it. Usage: pnpm --filter
 *     promise-infra run test262:promise-keyed node
 *     test/scripts/test262-promise-keyed-runner.mts --include 'allSettledKeyed'
 *     node test/scripts/test262-promise-keyed-runner.mts --limit 20 --json
 *     /tmp/results.json
 */

import { readFileSync, writeFileSync } from 'node:fs'
import path from 'node:path'
import process from 'node:process'

import { errorMessage } from '@socketsecurity/lib-stable/errors/message'
import { getDefaultLogger } from '@socketsecurity/lib-stable/logger/default'
import { spawnSync } from '@socketsecurity/lib-stable/process/spawn/child'

import { interpret } from 'local-temporal-infra/test/scripts/test262/classifier'
import {
  composeScript,
  walkTests,
} from 'local-temporal-infra/test/scripts/test262/harness'
import { parseFrontmatter } from 'local-temporal-infra/test/scripts/test262/parser'
import { report } from 'local-temporal-infra/test/scripts/test262/report'
import type {
  Test,
  Test262Result,
  TestCase,
} from 'local-temporal-infra/test/scripts/test262/types'

import { getLatestFinalBinary } from '../../../node-smol-builder/test/paths.mts'

import {
  TEST262_ALLOWLIST_PATH,
  TEST262_PROMISE_BUILTINS_DIR,
  TEST262_ROOT,
} from '../../lib/paths.mts'

const logger = getDefaultLogger()

/**
 * The keyed subset inside `built-ins/Promise/`. Anchored on a path segment so
 * a future `allKeyedSomethingElse/` directory does not silently join the gate.
 */
const KEYED_SUBSET_RE = /(?:^|[/\\])(?:allKeyed|allSettledKeyed)(?:[/\\]|$)/

/**
 * `doneprintHandle.js` prints exactly one of these to stdout when an async
 * test finishes. They are the verdict for a `flags: [async]` test — the
 * process exits 0 either way.
 */
const ASYNC_PASS = 'Test262:AsyncTestComplete'
const ASYNC_FAIL = 'Test262:AsyncTestFailure'

export function isKeyedSubsetPath(relativePath: string): boolean {
  return KEYED_SUBSET_RE.test(relativePath)
}

/**
 * Did an async test fail? A missing verdict counts as a failure: it means the
 * test neither completed nor reported, which is what a hang or an unhandled
 * rejection looks like from out here.
 */
export function asyncRunFailed(stdout: string): boolean {
  if (stdout.includes(ASYNC_FAIL)) {
    return true
  }
  return !stdout.includes(ASYNC_PASS)
}

/**
 * A module test cannot be fed through `-e`, so it is skipped rather than
 * counted. Async tests are NOT skipped here — the async-aware verdict below
 * is the whole reason this runner exists separately.
 */
export function shouldSkipKeyed(test: TestCase): string | undefined {
  if (test.attrs.module) {
    return 'module (not yet supported via -e)'
  }
  return undefined
}

export function loadAllowlist(filePath: string): string[] {
  let text: string
  try {
    text = readFileSync(filePath, 'utf8')
  } catch {
    return []
  }
  return text
    .split(/\r?\n/)
    .map(line => line.trim())
    .filter(line => line.length > 0 && !line.startsWith('#'))
}

export function resolveBinary(override?: string | undefined): string {
  if (override) {
    return override
  }
  const candidate = getLatestFinalBinary()
  if (!candidate) {
    throw new Error(
      'A node-smol binary for the currently pinned Node version was not found.\n' +
        'Run `pnpm --filter node-smol-builder run build` first, ' +
        'or pass --binary <path>.',
    )
  }
  return candidate
}

export function runOneKeyedTest(
  test: TestCase,
  scenario: 'strict' | 'sloppy' | 'raw',
  binary: string,
): Test {
  const script =
    scenario === 'raw' ? test.source : composeScript(test, scenario)
  const result = spawnSync(binary, ['-e', script], {
    encoding: 'utf8',
    timeout: 10_000,
    maxBuffer: 4 * 1024 * 1024,
  })
  const stderr = result.stderr ?? ''
  const stdout = result.stdout ?? ''

  // A sync test fails by throwing; an async test fails by printing its
  // failure verdict (or by printing nothing at all).
  const actualError = test.attrs.async
    ? result.status !== 0 || asyncRunFailed(stdout)
    : result.status !== 0 ||
      stderr.length > 0 ||
      stdout.includes('Test262Error')

  const expectedError = test.attrs.negative !== undefined

  let detail: string | undefined
  if (expectedError !== actualError) {
    detail = (stderr || stdout).slice(0, 400)
  }

  return { file: test.file, scenario, expectedError, actualError, detail }
}

// ── CLI ────────────────────────────────────────────────────────────

type ParsedArgs = {
  include?: string | undefined
  limit?: number | undefined
  json?: string | undefined
  binary?: string | undefined
  verbose: boolean
  allowlist?: string | undefined
}

export function parseArgs(argv: readonly string[]): ParsedArgs {
  const opts: ParsedArgs = { verbose: false }
  for (let i = 0; i < argv.length; i++) {
    const arg = argv[i]
    if (arg === '--include' && i + 1 < argv.length) {
      opts.include = argv[++i]
    } else if (arg === '--limit' && i + 1 < argv.length) {
      opts.limit = Number.parseInt(argv[++i]!, 10)
    } else if (arg === '--json' && i + 1 < argv.length) {
      opts.json = argv[++i]
    } else if (arg === '--binary' && i + 1 < argv.length) {
      opts.binary = argv[++i]
    } else if (arg === '--allowlist' && i + 1 < argv.length) {
      opts.allowlist = argv[++i]
    } else if (arg === '--verbose' || arg === '-v') {
      opts.verbose = true
    } else if (arg === '--help' || arg === '-h') {
      printHelp()
      process.exit(0)
    }
  }
  return opts
}

export function printHelp(): void {
  // oxlint-disable-next-line socket/no-logger-newline-literal -- help text is a single readable block; splitting into 12 logger.log calls would obscure structure.
  logger.log(`
Test262 Keyed Promise Combinator Subset Runner

Usage:
  node test/scripts/test262-promise-keyed-runner.mts [options]

Options:
  --include <regex>     Only run tests whose path matches this regex
  --limit <n>           Run at most N tests (after filtering)
  --json <path>         Write a JSON report to <path>
  --binary <path>       Path to the Node.js binary (default: built node-smol)
  --allowlist <path>    Path to a known-failures allowlist file
                        (default: test262-config/test262.allowlist)
  --verbose, -v         Print per-test classification inline
  --help, -h            Show this message
`)
}

// ── Main ───────────────────────────────────────────────────────────

function main(): void {
  const args = parseArgs(process.argv.slice(2))
  let binary: string
  try {
    binary = resolveBinary(args.binary)
  } catch (e) {
    logger.error(errorMessage(e))
    process.exit(1)
  }

  const allowlist = loadAllowlist(args.allowlist ?? TEST262_ALLOWLIST_PATH)

  logger.log('Test262 Keyed Promise Combinator Subset Runner')
  logger.log(`Binary:    ${binary}`)
  logger.log(`Corpus:    ${TEST262_ROOT}`)
  logger.log(`Allowlist: ${allowlist.length} entries`)
  logger.log('')

  const includeRe = args.include ? new RegExp(args.include, 'i') : undefined
  const startTime = Date.now()

  const candidates: string[] = []
  // walkTests is a generator, so there is no length to cache.
  // oxlint-disable-next-line socket/prefer-cached-for-loop -- generator
  for (const filePath of walkTests(TEST262_PROMISE_BUILTINS_DIR)) {
    const file = path.relative(TEST262_ROOT, filePath)
    if (!isKeyedSubsetPath(file)) {
      continue
    }
    if (includeRe && !includeRe.test(file)) {
      continue
    }
    candidates.push(filePath)
    if (args.limit && candidates.length >= args.limit) {
      break
    }
  }
  logger.log(`Tests to run: ${candidates.length}`)

  // A corpus that is not checked out is a skip, not a failure: the sparse
  // pattern lives in .gitmodules and a fresh clone has not materialized it
  // yet. `verify = pnpm --filter promise-infra test262:promise-keyed` on the
  // submodule entry is what makes the checked-out case get exercised.
  if (candidates.length === 0) {
    logger.warn(
      `No keyed-combinator tests found under ${TEST262_PROMISE_BUILTINS_DIR}.`,
    )
    logger.warn(
      'The test262 submodule is pinned + sparse-checked-out in .gitmodules; ' +
        'run `node scripts/fleet/git-partial-submodule.mts` (or the repo ' +
        'bootstrap) to materialize it, then re-run.',
    )
    process.exit(0)
  }

  const results: Test262Result[] = []
  for (let i = 0, { length } = candidates; i < length; i += 1) {
    const filePath = candidates[i]!
    const file = path.relative(TEST262_ROOT, filePath)
    const source = readFileSync(filePath, 'utf8')
    const attrs = parseFrontmatter(source)
    const test: TestCase = { filePath, file, source, attrs }

    const skipReason = shouldSkipKeyed(test)
    if (skipReason) {
      results.push({ skip: true, file, reason: skipReason })
      continue
    }

    const scenarios: Array<'strict' | 'sloppy' | 'raw'> = []
    if (attrs.raw) {
      scenarios.push('raw')
    } else if (attrs.onlyStrict) {
      scenarios.push('strict')
    } else if (attrs.noStrict) {
      scenarios.push('sloppy')
    } else {
      scenarios.push('strict', 'sloppy')
    }

    for (let j = 0, { length: jlen } = scenarios; j < jlen; j += 1) {
      const scenario = scenarios[j]!
      const result = runOneKeyedTest(test, scenario, binary)
      results.push(result)
      if (args.verbose && result.expectedError !== result.actualError) {
        logger.warn(`  [${scenario}] ${file}: ${result.detail?.slice(0, 200)}`)
      }
    }

    if (i > 0 && i % 25 === 0) {
      const elapsed = ((Date.now() - startTime) / 1000).toFixed(1)
      logger.info(`Progress: ${i}/${length} (${elapsed}s)`)
    }
  }

  const summary = interpret(results, allowlist, Date.now() - startTime)
  report(summary)

  if (args.json) {
    writeFileSync(args.json, JSON.stringify(summary, null, 2))
    logger.log(`JSON report: ${args.json}`)
  }

  process.exit(summary.passed ? 0 : 1)
}

// Only invoke main() when run directly, not when imported by the vitest unit
// test that exercises the pure filters. Without this guard an import would
// walk the corpus and spawn the binary.
if (import.meta.main) {
  main()
}
