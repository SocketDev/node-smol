import { afterEach, expect, it, vi } from 'vitest'
import { mkdtemp, mkdir, readFile, rm, writeFile } from 'node:fs/promises'
import os from 'node:os'
import path from 'node:path'

const fixture = vi.hoisted(() => ({
  upstream: '',
  sha: '1234567890123456789012345678901234567890',
  createCheckpoint: vi.fn(),
}))

vi.mock('../../scripts/source-copied/shared/paths.mts', () => ({
  get UPSTREAM_PATH() { return fixture.upstream },
}))
vi.mock('build-infra/lib/build-steps', () => ({
  cleanCheckpoint: vi.fn(),
  createCheckpoint: fixture.createCheckpoint,
}))
vi.mock('build-infra/lib/checkpoint-manager', () => ({
  getCheckpointData: vi.fn(),
  shouldRun: vi.fn().mockResolvedValue(true),
}))
vi.mock('@socketsecurity/lib-stable/process/spawn/child', () => ({
  spawn: vi.fn(async () => ({ code: 0, stdout: fixture.sha })),
}))

import { recoverNodeSourceCheckpoint, verifyNodeUpstreamSha } from '../../scripts/source-copied/shared/copy-source.mts'

let temporaryRoot = ''

it('rejects a missing upstream pin', async () => {
  await expect(verifyNodeUpstreamSha('unused', undefined)).rejects.toBeInstanceOf(Error)
})

it('rejects a different upstream revision before recording a checkpoint', async () => {
  await expect(recoverNodeSourceCheckpoint({
    nodeSha: 'abcdefabcdefabcdefabcdefabcdefabcdefabcd',
  })).rejects.toBeInstanceOf(Error)
  expect(fixture.createCheckpoint).not.toHaveBeenCalled()
})

afterEach(async () => {
  if (temporaryRoot) await rm(temporaryRoot, { recursive: true, force: true })
  vi.clearAllMocks()
})

it('replaces unverified source with pinned upstream before recording a checkpoint', async () => {
  temporaryRoot = await mkdtemp(path.join(os.tmpdir(), 'source-recovery-'))
  fixture.upstream = path.join(temporaryRoot, 'upstream')
  const sharedSourceDir = path.join(temporaryRoot, 'source')
  await mkdir(fixture.upstream)
  await mkdir(sharedSourceDir)
  await writeFile(path.join(fixture.upstream, 'configure'), 'pinned source')
  await writeFile(path.join(sharedSourceDir, 'configure'), 'stale source')
  await writeFile(path.join(sharedSourceDir, 'stale-only.txt'), 'discard')
  fixture.createCheckpoint.mockImplementation(async () => {
    expect(await readFile(path.join(sharedSourceDir, 'configure'), 'utf8')).toBe('pinned source')
  })

  await recoverNodeSourceCheckpoint({
    nodeSha: fixture.sha,
    nodeVersion: '26.13.0',
    packageName: 'source-fixture',
    sharedBuildDir: temporaryRoot,
    sharedSourceDir,
  })

  expect(fixture.createCheckpoint).toHaveBeenCalledOnce()
  await expect(readFile(path.join(sharedSourceDir, 'stale-only.txt'))).rejects.toMatchObject({ code: 'ENOENT' })
})
