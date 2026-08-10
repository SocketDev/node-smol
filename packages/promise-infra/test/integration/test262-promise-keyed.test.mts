/**
 * @file Test262 keyed-combinator conformance gate (vitest wrapper).
 *   Spawns the CLI runner at
 *   test/scripts/test262-promise-keyed-runner.mts and asserts exit code 0. The
 *   runner walks the keyed subset of the pinned test262 corpus, runs each test
 *   through node-smol, classifies against test262-config/test262.allowlist, and
 *   exits non-zero on regression OR stale allowlist entry.
 *   Why spawn-and-check rather than inline `test.each`: the runner has rich CLI
 *   flags (--include, --limit, --json, --binary) worth preserving for dev
 *   debugging, and failures classify against an allowlist so only unexpected
 *   regressions should fail the gate. Vitest's per-test reporter does not
 *   compose with that classification cleanly; the runner's own report does.
 */

import path from 'node:path'
import { fileURLToPath } from 'node:url'

import { describe, expect, it } from 'vitest'

import { spawn } from '@socketsecurity/lib-stable/process/spawn/child'

import { getLatestFinalBinary } from '../../../node-smol-builder/test/paths.mts'

const __dirname = path.dirname(fileURLToPath(import.meta.url))
const RUNNER = path.resolve(
  __dirname,
  '..',
  'scripts',
  'test262-promise-keyed-runner.mts',
)

// Gate the suite on the node-smol Final/ binary being present — the runner
// spawns a subprocess against it for each test. CI / fresh checkouts without a
// build will skip.
const skipTests = !getLatestFinalBinary()

// 89 keyed tests × up to 2 scenarios, each a process spawn. Budget generous.
const TIMEOUT_MS = 10 * 60 * 1000

describe.skipIf(skipTests)(
  'Test262 keyed promise combinator conformance',
  () => {
    it(
      'no unexpected failures vs test262.allowlist',
      async () => {
        const result = await spawn('node', [RUNNER], { stdio: 'inherit' })
        expect(result.code).toBe(0)
      },
      TIMEOUT_MS,
    )
  },
)
