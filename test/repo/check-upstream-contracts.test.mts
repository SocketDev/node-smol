import assert from 'node:assert/strict'
import { mkdtempSync, mkdirSync, rmSync, writeFileSync } from 'node:fs'
import os from 'node:os'
import path from 'node:path'
import { execFileSync } from 'node:child_process'
import test from 'node:test'

import { collectContractErrors } from '../../scripts/repo/check-upstream-contracts.mts'

function git(args: string[], cwd: string): void {
  execFileSync('git', args, { cwd, stdio: 'ignore' })
}

function makeFixture(): string {
  const root = mkdtempSync(path.join(os.tmpdir(), 'node-smol-contract-'))
  const upstream = path.join(root, 'upstream', 'stuie')
  mkdirSync(path.join(upstream, 'crates', 'stuie'), { recursive: true })
  mkdirSync(path.join(upstream, 'napi', 'stuie'), { recursive: true })
  mkdirSync(path.join(upstream, 'fixtures'), { recursive: true })
  writeFileSync(path.join(upstream, 'fixtures', 'mouse-events.golden.json'), '{}')
  git(['init', '--quiet'], upstream)
  git(['config', 'user.email', 'test@example.com'], upstream)
  git(['config', 'user.name', 'Test'], upstream)
  git(['add', '.'], upstream)
  git(['commit', '--quiet', '-m', 'fixture'], upstream)
  return root
}

test('reports a drifted upstream revision', () => {
  const root = makeFixture()
  try {
    assert.match(collectContractErrors(root).join('\n'), /pinned .* expected/)
  } finally {
    rmSync(root, { force: true, recursive: true })
  }
})

test('reports a missing upstream checkout', () => {
  const root = mkdtempSync(path.join(os.tmpdir(), 'node-smol-contract-'))
  try {
    assert.deepEqual(collectContractErrors(root), [
      'decmpfs: missing upstream checkout',
      'stuie: missing upstream checkout',
    ])
  } finally {
    rmSync(root, { force: true, recursive: true })
  }
})
