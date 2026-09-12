import { mkdirSync, mkdtempSync, symlinkSync, writeFileSync } from 'node:fs'
import os from 'node:os'
import path from 'node:path'
import { safeDelete } from '@socketsecurity/lib-stable/fs/safe'
import { afterEach, beforeEach, expect, test, vi } from 'vitest'

const mocks = vi.hoisted(() => ({ spawn: vi.fn(), resolve: vi.fn() }))
vi.mock(
  import('@socketsecurity/lib-stable/process/spawn/child'),
  async original => ({ ...(await original()), spawnSync: mocks.spawn }),
)
vi.mock(
  import('../../../../../scripts/fleet/gen/gitmodules-hash.mts'),
  async original => ({ ...(await original()), resolveAll: mocks.resolve }),
)

import {
  assertStuieSourcePath,
  readStuieSourcePin,
  stuieGitEnvironment,
  verifyStuieSource,
} from '../../../../../scripts/repo/check/smol-tui-cabi-symbols-are-mapped/source.mts'

const revision = '1234567890abcdef1234567890abcdef12345678'
const checksum = 'a'.repeat(64)
let temporary: string
let repoRoot: string
let source: string

beforeEach(() => {
  temporary = mkdtempSync(path.join(os.tmpdir(), 'stuie-source-'))
  repoRoot = path.join(temporary, 'repo')
  source = path.join(repoRoot, 'upstream', 'stuie')
  mkdirSync(source, { recursive: true })
  writeFileSync(
    path.join(repoRoot, '.gitmodules'),
    `# stuie-example sha256:${checksum}\n[submodule "upstream/stuie"]\n path = upstream/stuie\n url = git@github.com:SocketDev/stuie.git\n ref = ${revision}\n`,
  )
  mocks.spawn
    .mockReturnValueOnce({ status: 0, stdout: revision })
    .mockReturnValue({ status: 0, stdout: '' })
  mocks.resolve.mockResolvedValue([{ computed: checksum }])
})

afterEach(async () => {
  vi.unstubAllEnvs()
  vi.resetAllMocks()
  await safeDelete(temporary)
})

test('uses a contained immutable source pin', () => {
  expect(readStuieSourcePin(repoRoot)).toMatchObject({
    ref: revision,
    headerSha: checksum,
    path: 'upstream/stuie',
  })
  expect(assertStuieSourcePath(repoRoot, source)).toBe(source)
})

test('rejects a neighboring checkout even when its name matches', () => {
  const neighbor = path.join(temporary, 'stuie')
  mkdirSync(neighbor)
  expect(() => assertStuieSourcePath(repoRoot, neighbor)).toThrow(TypeError)
})

test('rejects an external source-directory symlink', () => {
  const outside = path.join(temporary, 'outside')
  mkdirSync(outside)
  symlinkSync(outside, path.join(repoRoot, 'external'))
  writeFileSync(
    path.join(repoRoot, '.gitmodules'),
    `# stuie-example sha256:${checksum}\n[submodule "upstream/stuie"]\n path = external\n url = git@github.com:SocketDev/stuie.git\n ref = ${revision}\n`,
  )
  expect(() => readStuieSourcePin(repoRoot)).toThrow(TypeError)
})

test('verifies revision, clean source, and checksum before snapshot updates', async () => {
  await expect(verifyStuieSource(repoRoot)).resolves.toBe(source)
  expect(mocks.resolve).toHaveBeenCalledOnce()
})

test('source verification removes inherited Git repository context', async () => {
  vi.stubEnv('GIT_DIR', '/outside/example.git')
  vi.stubEnv('GIT_WORK_TREE', '/outside/example')
  expect(stuieGitEnvironment()).not.toHaveProperty('GIT_DIR')
  expect(stuieGitEnvironment()).not.toHaveProperty('GIT_WORK_TREE')
  await verifyStuieSource(repoRoot)
  expect(mocks.spawn.mock.calls[0]?.[2]?.env).not.toHaveProperty('GIT_DIR')
  expect(mocks.spawn.mock.calls[1]?.[2]?.env).not.toHaveProperty('GIT_WORK_TREE')
})

test('rejects a changed source checksum', async () => {
  mocks.resolve.mockResolvedValue([{ computed: 'b'.repeat(64) }])
  await expect(verifyStuieSource(repoRoot)).rejects.toBeInstanceOf(TypeError)
})

test('rejects a different source revision before checksum resolution', async () => {
  mocks.spawn.mockReset().mockReturnValue({ status: 0, stdout: '0'.repeat(40) })
  await expect(verifyStuieSource(repoRoot)).rejects.toBeInstanceOf(TypeError)
  expect(mocks.resolve).not.toHaveBeenCalled()
})

test('missing immutable metadata cannot select source', () => {
  writeFileSync(
    path.join(repoRoot, '.gitmodules'),
    '[submodule "upstream/stuie"]\n path = upstream/stuie\n url = git@github.com:SocketDev/stuie.git\n branch = main\n',
  )
  expect(() => readStuieSourcePin(repoRoot)).toThrow(TypeError)
})

test('missing source directories cannot compare equal', () => {
  expect(() =>
    assertStuieSourcePath(repoRoot, path.join(repoRoot, 'missing')),
  ).toThrow(TypeError)
})

test('modified source is rejected before checksum resolution', async () => {
  mocks.spawn
    .mockReset()
    .mockReturnValueOnce({ status: 0, stdout: revision })
    .mockReturnValueOnce({
      status: 0,
      stdout: ' M crates/stuie-cabi/src/lib.rs',
    })
  await expect(verifyStuieSource(repoRoot)).rejects.toBeInstanceOf(TypeError)
  expect(mocks.resolve).not.toHaveBeenCalled()
})
