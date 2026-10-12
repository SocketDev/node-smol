import { beforeEach, describe, expect, it, vi } from 'vitest'
import {
  extractTarball,
  validateTarballPaths,
} from '../../../packages/build-infra/lib/tarball-utils.mts'

const { spawnMock } = vi.hoisted(() => ({ spawnMock: vi.fn() }))

vi.mock('@socketsecurity/lib-stable/process/spawn/child', () => ({
  spawn: spawnMock,
}))
vi.mock('@socketsecurity/lib-stable/fs/safe', () => ({
  safeMkdir: vi.fn(),
}))
vi.mock('../../../packages/build-infra/lib/which-required.mts', () => ({
  whichRequired: vi.fn().mockResolvedValue('tar'),
}))
vi.mock('../../../packages/build-infra/lib/platform-mappings.mts', () => ({
  tarSupportsNoAbsoluteNames: vi.fn().mockResolvedValue(false),
  tarSupportsOverwrite: vi.fn().mockResolvedValue(false),
}))

beforeEach(() => {
  spawnMock.mockReset()
  spawnMock.mockResolvedValue({ stdout: 'runtime/node.exe\n', code: 0 })
})

describe('native tar archive paths', () => {
  it.each([
    {
      name: 'Windows',
      archive: 'D:\\build\\runtime.tar.gz',
      expected: 'D:/build/runtime.tar.gz',
    },
    {
      name: 'POSIX',
      archive: '/build/runtime.tar.gz',
      expected: '/build/runtime.tar.gz',
    },
  ])('preserves $name paths when listing', async entry => {
    expect(await validateTarballPaths(entry.archive)).toEqual([
      'runtime/node.exe',
    ])
    expect(spawnMock).toHaveBeenCalledWith('tar', ['-tzf', entry.expected], {
      stdio: 'pipe',
    })
  })

  it.each([
    {
      name: 'Windows',
      archive: 'D:\\build\\runtime.tar.gz',
      destination: 'D:\\build\\extracted',
      normalizedArchive: 'D:/build/runtime.tar.gz',
      normalizedDestination: 'D:/build/extracted',
    },
    {
      name: 'POSIX',
      archive: '/build/runtime.tar.gz',
      destination: '/build/extracted',
      normalizedArchive: '/build/runtime.tar.gz',
      normalizedDestination: '/build/extracted',
    },
  ])('preserves $name paths when extracting', async entry => {
    expect(await extractTarball(entry.archive, entry.destination)).toEqual([
      'runtime/node.exe',
    ])
    expect(spawnMock).toHaveBeenNthCalledWith(
      2,
      'tar',
      ['-xzf', entry.normalizedArchive, '-C', entry.normalizedDestination],
      { stdio: 'ignore' },
    )
  })

  it('stops before extraction when listing fails', async () => {
    spawnMock.mockRejectedValueOnce(new Error('Archive listing failed.'))
    await expect(
      extractTarball('D:\\build\\runtime.tar.gz', 'D:\\build\\extracted'),
    ).rejects.toThrow(Error)
    expect(spawnMock).toHaveBeenCalledTimes(1)
    expect(spawnMock).toHaveBeenCalledWith(
      'tar',
      ['-tzf', 'D:/build/runtime.tar.gz'],
      { stdio: 'pipe' },
    )
  })
})
