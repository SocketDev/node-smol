/**
 * @file Unit tests for the keyed-combinator test262 runner's pure parts.
 *   Corpus walking and binary spawning stay in the runner and are exercised by
 *   running it; the three decisions that decide whether the gate MEANS
 *   anything are deterministic, so they are pinned here:
 *
 *   1. The subset filter — `built-ins/Promise/` holds every Promise test, so a
 *      wrong filter either runs the whole Promise suite or nothing at all.
 *   2. The async verdict — almost every keyed test reports through
 *      `doneprintHandle.js` on stdout and exits 0 either way, so reading the
 *      exit code alone would report a green gate that measured nothing.
 *   3. The allowlist loader — a missing file must read as zero entries, not throw,
 *      so a fresh clone reports honestly instead of crashing.
 */

import { mkdtempSync, writeFileSync } from 'node:fs'
import os from 'node:os'
import path from 'node:path'

import { afterEach, beforeEach, describe, expect, it } from 'vitest'
import { safeDelete } from '@socketsecurity/lib-stable/fs/safe'

import {
  asyncRunFailed,
  isKeyedSubsetPath,
  loadAllowlist,
  shouldSkipKeyed,
} from '../scripts/test262-promise-keyed-runner.mts'
import type { TestCase } from 'local-temporal-infra/test/scripts/test262/types'

let fixtureRoot: string

beforeEach(() => {
  fixtureRoot = mkdtempSync(path.join(os.tmpdir(), 'promise-keyed-test-'))
})

afterEach(async () => {
  await safeDelete(fixtureRoot)
})

function makeTestCase(overrides: Partial<TestCase>): TestCase {
  return {
    filePath: '/path/to/example/resolve-element-function-name.js',
    file: 'test/built-ins/Promise/allKeyed/resolve-element-function-name.js',
    source: '',
    attrs: {},
    ...overrides,
  }
}

describe('isKeyedSubsetPath', () => {
  it('accepts both keyed directories', () => {
    expect(
      isKeyedSubsetPath('test/built-ins/Promise/allKeyed/prop-desc.js'),
    ).toBe(true)
    expect(
      isKeyedSubsetPath(
        'test/built-ins/Promise/allSettledKeyed/invoke-then.js',
      ),
    ).toBe(true)
  })

  it('rejects the rest of the Promise suite', () => {
    expect(isKeyedSubsetPath('test/built-ins/Promise/all/prop-desc.js')).toBe(
      false,
    )
    expect(
      isKeyedSubsetPath('test/built-ins/Promise/allSettled/prop-desc.js'),
    ).toBe(false)
    expect(isKeyedSubsetPath('test/built-ins/Promise/race/name.js')).toBe(false)
  })

  it('anchors on a whole path segment', () => {
    // A future sibling directory whose name merely starts with `allKeyed`
    // must not join this gate by accident.
    expect(
      isKeyedSubsetPath(
        'test/built-ins/Promise/allKeyedSomethingElse/example.js',
      ),
    ).toBe(false)
  })

  it('matches Windows-style separators', () => {
    expect(
      isKeyedSubsetPath('test\\built-ins\\Promise\\allKeyed\\prop-desc.js'),
    ).toBe(true)
  })
})

describe('asyncRunFailed', () => {
  it('passes on the completion marker', () => {
    expect(asyncRunFailed('Test262:AsyncTestComplete\n')).toBe(false)
  })

  it('fails on the failure marker', () => {
    expect(
      asyncRunFailed('Test262:AsyncTestFailure:Test262Error: expected 1\n'),
    ).toBe(true)
  })

  it('fails on silence', () => {
    // No verdict means the test neither completed nor reported — a hang or an
    // unhandled rejection. Treating that as a pass is how a gate goes green
    // while measuring nothing.
    expect(asyncRunFailed('')).toBe(true)
  })

  it('fails when both markers appear', () => {
    expect(
      asyncRunFailed('Test262:AsyncTestComplete\nTest262:AsyncTestFailure\n'),
    ).toBe(true)
  })
})

describe('shouldSkipKeyed', () => {
  it('does not skip an async test', () => {
    expect(shouldSkipKeyed(makeTestCase({ attrs: { async: true } }))).toBe(
      undefined,
    )
  })

  it('skips a module test', () => {
    expect(shouldSkipKeyed(makeTestCase({ attrs: { module: true } }))).toBe(
      'module (not yet supported via -e)',
    )
  })
})

describe('loadAllowlist', () => {
  it('reads entries and drops comments and blanks', () => {
    const file = path.join(fixtureRoot, 'test262.allowlist')
    writeFileSync(
      file,
      [
        '# a comment',
        '',
        'test/built-ins/Promise/allKeyed/prop-desc.js (strict)',
        '   ',
        'test/built-ins/Promise/allKeyed/prop-desc.js (sloppy)',
      ].join('\n'),
    )
    expect(loadAllowlist(file)).toEqual([
      'test/built-ins/Promise/allKeyed/prop-desc.js (strict)',
      'test/built-ins/Promise/allKeyed/prop-desc.js (sloppy)',
    ])
  })

  it('reads a missing file as zero entries', () => {
    expect(loadAllowlist(path.join(fixtureRoot, 'absent.allowlist'))).toEqual(
      [],
    )
  })
})
