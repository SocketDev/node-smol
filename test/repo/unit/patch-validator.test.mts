import path from 'node:path'

import { beforeEach, describe, expect, it, vi } from 'vitest'

import {
  applyPatch,
  resolvePatchExecutable,
  revertPatch,
  testPatchApplication,
  validatePatch,
} from '../../../packages/build-infra/lib/patch-validator.mts'

const { spawnMock, whichMock, isSpawnErrorMock } = vi.hoisted(() => ({
  isSpawnErrorMock: vi.fn(),
  spawnMock: vi.fn(),
  whichMock: vi.fn(),
}))

vi.mock('@socketsecurity/lib-stable/process/spawn/child', () => ({
  spawn: spawnMock,
}))
vi.mock('@socketsecurity/lib-stable/exe/path/which', () => ({
  which: whichMock,
}))
vi.mock('@socketsecurity/lib-stable/process/spawn/errors', () => ({
  isSpawnError: isSpawnErrorMock,
}))

beforeEach(() => {
  spawnMock.mockReset()
  whichMock.mockReset()
  isSpawnErrorMock.mockReset()
  whichMock.mockResolvedValue('/tools/patch')
  isSpawnErrorMock.mockImplementation(
    (error: unknown) =>
      typeof error === 'object' && error !== null && 'code' in error,
  )
})

function mockPatchSpawn(
  outcome: { code: number; stderr?: string; stdout?: string } = { code: 0 },
): void {
  spawnMock.mockImplementation((command: string, args: string[]) => {
    void command
    if (args[0] === '--version') {
      return Promise.resolve({ code: 0, stdout: 'GNU patch 2.7.6' })
    }
    return Promise.resolve(outcome)
  })
}

function mockPatchSpawnRejected(outcome: unknown): void {
  spawnMock.mockImplementation((command: string, args: string[]) => {
    void command
    if (args[0] === '--version') {
      return Promise.resolve({ code: 0, stdout: 'GNU patch 2.7.6' })
    }
    return Promise.reject(outcome)
  })
}

describe('resolvePatchExecutable', () => {
  it('uses the first PATH result on Unix without a version probe', async () => {
    whichMock.mockResolvedValue('/tools/first-patch')

    await expect(
      resolvePatchExecutable({
        platform: 'linux',
        locate: whichMock,
        probe: spawnMock,
      }),
    ).resolves.toBe('/tools/first-patch')
    expect(whichMock).toHaveBeenCalledWith('patch', { nothrow: true })
    expect(spawnMock).not.toHaveBeenCalled()
  })

  it('skips old Strawberry patch and selects supported Git patch', async () => {
    const strawberryPatch = 'C:\\Strawberry\\c\\bin\\patch.exe'
    const gitPatch = 'C:\\Program Files\\Git\\usr\\bin\\patch.exe'
    whichMock.mockResolvedValue([strawberryPatch, gitPatch])
    spawnMock.mockImplementation((candidate: string) =>
      Promise.resolve({
        code: 0,
        stdout:
          candidate === strawberryPatch ? 'GNU patch 2.5.9' : 'GNU patch 2.7.6',
      }),
    )

    await expect(
      resolvePatchExecutable({
        platform: 'win32',
        locate: whichMock,
        probe: spawnMock,
      }),
    ).resolves.toBe(gitPatch)
    expect(whichMock).toHaveBeenCalledWith('patch', {
      all: true,
      nothrow: true,
    })
    expect(spawnMock).toHaveBeenNthCalledWith(
      1,
      strawberryPatch,
      ['--version'],
      expect.objectContaining({
        encoding: 'utf8',
        maxBuffer: 64 * 1024,
        timeout: 5000,
      }),
    )
    expect(spawnMock).toHaveBeenNthCalledWith(
      2,
      gitPatch,
      ['--version'],
      expect.objectContaining({
        encoding: 'utf8',
        maxBuffer: 64 * 1024,
        timeout: 5000,
      }),
    )
  })

  it('rejects when patch is missing from Windows PATH', async () => {
    whichMock.mockResolvedValue(null)

    await expect(
      resolvePatchExecutable({
        platform: 'win32',
        locate: whichMock,
        probe: spawnMock,
      }),
    ).rejects.toThrow(/Install GNU patch 2\.7 or newer/)
    expect(spawnMock).not.toHaveBeenCalled()
  })

  it('rejects when Windows PATH contains only unsupported versions', async () => {
    whichMock.mockResolvedValue(['C:\\Strawberry\\c\\bin\\patch.exe'])
    spawnMock.mockResolvedValue({ code: 0, stdout: 'GNU patch 2.5.9' })

    await expect(
      resolvePatchExecutable({
        platform: 'win32',
        locate: whichMock,
        probe: spawnMock,
      }),
    ).rejects.toThrow(/Install GNU patch 2\.7 or newer/)
  })

  it('rejects when all Windows version probes fail', async () => {
    whichMock.mockResolvedValue([
      'C:\\Tools\\first\\patch.exe',
      'C:\\Tools\\second\\patch.exe',
    ])
    spawnMock.mockRejectedValue(new Error('version probe failed'))

    await expect(
      resolvePatchExecutable({
        platform: 'win32',
        locate: whichMock,
        probe: spawnMock,
      }),
    ).rejects.toThrow(/Install GNU patch 2\.7 or newer/)
    expect(spawnMock).toHaveBeenCalledTimes(2)
  })
})

describe('patch executable callers', () => {
  it('uses the resolver for apply, reverse, dry-run, and validation', async () => {
    mockPatchSpawn()

    await applyPatch('changes.patch', '/source')
    await revertPatch('changes.patch', '/source')
    await expect(
      testPatchApplication('changes.patch', '/source'),
    ).resolves.toBe(true)
    await expect(validatePatch('changes.patch', '/source')).resolves.toBe(true)

    expect(whichMock).toHaveBeenCalledTimes(4)
    expect(spawnMock).toHaveBeenCalledWith(
      '/tools/patch',
      ['-p1', '--batch', '-i', path.resolve('changes.patch')],
      expect.objectContaining({ cwd: '/source' }),
    )
    expect(spawnMock).toHaveBeenCalledWith(
      '/tools/patch',
      ['-p1', '--batch', '--reverse', '-i', path.resolve('changes.patch')],
      expect.objectContaining({ cwd: '/source' }),
    )
    expect(spawnMock).toHaveBeenCalledWith(
      '/tools/patch',
      [
        '-p1',
        '--batch',
        '--dry-run',
        '--reverse',
        '-i',
        path.resolve('changes.patch'),
      ],
      expect.objectContaining({ cwd: '/source' }),
    )
    expect(spawnMock).toHaveBeenCalledWith(
      '/tools/patch',
      ['-p1', '--batch', '--dry-run', '-i', path.resolve('changes.patch')],
      expect.objectContaining({ cwd: '/source' }),
    )
  })
})

describe('applyPatch diagnostics', () => {
  it('reports both output streams when the patch command exits unsuccessfully', async () => {
    mockPatchSpawn({
      code: 2,
      stderr: 'patch context did not match',
      stdout: 'checking file source.c',
    })

    await expect(applyPatch('changes.patch', '/source')).rejects.toThrow(
      /exit 2[\s\S]*stderr: patch context did not match[\s\S]*stdout: checking file source\.c/,
    )
  })

  it('reports output streams from a rejected spawn error', async () => {
    mockPatchSpawnRejected({
      code: 1,
      stderr: 'file could not be opened',
      stdout: 'patch aborted',
    })

    await expect(applyPatch('changes.patch', '/source')).rejects.toThrow(
      /stderr: file could not be opened[\s\S]*stdout: patch aborted/,
    )
  })

  it('does not throw when the patch command succeeds', async () => {
    mockPatchSpawn()

    await expect(
      applyPatch('changes.patch', '/source'),
    ).resolves.toBeUndefined()
    expect(spawnMock).toHaveBeenCalledWith(
      '/tools/patch',
      ['-p1', '--batch', '-i', path.resolve('changes.patch')],
      expect.objectContaining({ cwd: '/source' }),
    )
  })

  it('limits the amount of captured output in the error', async () => {
    mockPatchSpawn({ code: 1, stderr: 'x'.repeat(6_000) })

    let message = ''
    try {
      await applyPatch('changes.patch', '/source')
    } catch (error) {
      message = error instanceof Error ? error.message : String(error)
    }

    expect(message).toContain('[4000 characters omitted]')
    expect(message.length).toBeLessThan(2_200)
  })
})
