import { beforeEach, describe, expect, it, vi } from 'vitest'
import {
  extractTarball,
  validateTarballPaths,
} from '../../../packages/build-infra/lib/tarball-utils.mts'

const { spawnMock, whichMock } = vi.hoisted(() => ({
  spawnMock: vi.fn(),
  whichMock: vi.fn(),
}))

vi.mock('@socketsecurity/lib-stable/process/spawn/child', () => ({
  spawn: spawnMock,
}))
vi.mock('@socketsecurity/lib-stable/fs/safe', () => ({
  safeMkdir: vi.fn(),
}))
vi.mock('../../../packages/build-infra/lib/which-required.mts', () => ({
  whichRequired: whichMock,
}))

let tarHelpOutput = ''
let tarHelpResultCode = 0
let tarListingResult = { stdout: 'runtime/node.exe\n', stderr: '', code: 0 }
let tarListingError: Error | undefined

beforeEach(() => {
  spawnMock.mockReset()
  whichMock.mockReset()
  whichMock.mockResolvedValue('tar')
  tarHelpOutput = ''
  tarHelpResultCode = 0
  tarListingResult = { stdout: 'runtime/node.exe\n', stderr: '', code: 0 }
  tarListingError = undefined
  spawnMock.mockImplementation((...spawnArgs) => {
    const executable = spawnArgs[0] as string
    const args = spawnArgs[1] as string[]
    if (!executable) {
      return Promise.reject(new Error('tar executable is missing'))
    }
    if (args[0] === '--help') {
      return Promise.resolve({
        stdout: tarHelpOutput,
        stderr: '',
        code: tarHelpResultCode,
      })
    }
    if (tarListingError) {
      return Promise.reject(tarListingError)
    }
    return Promise.resolve(tarListingResult)
  })
})

describe('native tar archive paths', () => {
  it.each([
    {
      name: 'GNU tar with a Windows drive path',
      executable: 'C:/Program Files/Git/usr/bin/tar.EXE',
      archive: 'D:\\build artifacts\\runtime.tar.gz',
      destination: 'D:\\build artifacts\\extracted',
      normalizedArchive: 'D:/build artifacts/runtime.tar.gz',
      normalizedDestination: 'D:/build artifacts/extracted',
      help: '--force-local\n--no-absolute-names\n--overwrite',
    },
    {
      name: 'BSD tar with a Windows drive path',
      executable: 'C:/Windows/System32/tar.exe',
      archive: 'D:\\build\\runtime.tar.gz',
      destination: 'D:\\build\\extracted',
      normalizedArchive: 'D:/build/runtime.tar.gz',
      normalizedDestination: 'D:/build/extracted',
      help: 'tar -k keeps the old file',
    },
    {
      name: 'GNU tar with a Windows UNC path',
      executable: 'C:/Program Files/Git/usr/bin/tar.EXE',
      archive: '\\\\server\\share\\build artifacts\\runtime.tar.gz',
      destination: '\\\\server\\share\\build artifacts\\extracted',
      normalizedArchive: '//server/share/build artifacts/runtime.tar.gz',
      normalizedDestination: '//server/share/build artifacts/extracted',
      help: '--force-local\n--no-absolute-names\n--overwrite',
    },
    {
      name: 'GNU tar with a POSIX path',
      executable: '/usr/bin/gtar',
      archive: '/build artifacts/runtime.tar.gz',
      destination: '/build artifacts/extracted',
      normalizedArchive: '/build artifacts/runtime.tar.gz',
      normalizedDestination: '/build artifacts/extracted',
      help: '--force-local\n--no-absolute-names\n--overwrite',
    },
    {
      name: 'BSD tar with a POSIX path',
      executable: '/usr/bin/tar',
      archive: '/build/runtime.tar.gz',
      destination: '/build/extracted',
      normalizedArchive: '/build/runtime.tar.gz',
      normalizedDestination: '/build/extracted',
      help: 'tar -k keeps the old file',
    },
  ])(
    'uses the selected $name executable for listing and extraction',
    async entry => {
      whichMock.mockResolvedValue(entry.executable)
      tarHelpOutput = entry.help
      expect(await extractTarball(entry.archive, entry.destination)).toEqual([
        'runtime/node.exe',
      ])
      expect(whichMock).toHaveBeenCalledTimes(1)
      expect(whichMock).toHaveBeenCalledWith('tar')
      expect(spawnMock).toHaveBeenNthCalledWith(
        1,
        entry.executable,
        ['--help'],
        {
          killSignal: 'SIGKILL',
          maxBuffer: 64 * 1024,
          stdio: 'pipe',
          timeout: 5000,
        },
      )
      expect(spawnMock).toHaveBeenNthCalledWith(
        2,
        entry.executable,
        [
          ...(entry.help.includes('--force-local') ? ['--force-local'] : []),
          '-tzf',
          entry.normalizedArchive,
        ],
        { stdio: 'pipe' },
      )
      expect(spawnMock).toHaveBeenNthCalledWith(
        3,
        entry.executable,
        [
          ...(entry.help.includes('--force-local') ? ['--force-local'] : []),
          '-xzf',
          entry.normalizedArchive,
          '-C',
          entry.normalizedDestination,
          ...(entry.help.includes('--no-absolute-names')
            ? ['--no-absolute-names']
            : []),
          ...(entry.help.includes('--overwrite') ? ['--overwrite'] : []),
        ],
        { stdio: 'ignore' },
      )
    },
  )

  it('stops before extraction when listing fails', async () => {
    tarListingResult = { stdout: '', stderr: 'failed', code: 2 }
    await expect(
      extractTarball('D:\\build\\runtime.tar.gz', 'D:\\build\\extracted'),
    ).rejects.toThrow('tar listing failed with exit code 2')
    expect(spawnMock).toHaveBeenCalledTimes(2)
    expect(spawnMock).toHaveBeenNthCalledWith(1, 'tar', ['--help'], {
      killSignal: 'SIGKILL',
      maxBuffer: 64 * 1024,
      stdio: 'pipe',
      timeout: 5000,
    })
    expect(spawnMock).toHaveBeenNthCalledWith(
      2,
      'tar',
      ['-tzf', 'D:/build/runtime.tar.gz'],
      { stdio: 'pipe' },
    )
  })

  it('stops before extraction when listing rejects', async () => {
    tarListingError = new Error('Archive listing could not start')
    await expect(
      extractTarball('/build/runtime.tar.gz', '/build/extracted'),
    ).rejects.toThrow('Archive listing could not start')
    expect(spawnMock).toHaveBeenCalledTimes(2)
  })

  it('uses no capability flags when the selected tar help command fails', async () => {
    whichMock.mockResolvedValue('/usr/bin/unsupported-tar')
    tarHelpOutput = '--force-local\n--no-absolute-names\n--overwrite'
    tarHelpResultCode = 2

    await extractTarball('/build/runtime.tar.gz', '/build/extracted')

    expect(spawnMock).toHaveBeenNthCalledWith(
      1,
      '/usr/bin/unsupported-tar',
      ['--help'],
      {
        killSignal: 'SIGKILL',
        maxBuffer: 64 * 1024,
        stdio: 'pipe',
        timeout: 5000,
      },
    )
    expect(spawnMock).toHaveBeenNthCalledWith(
      2,
      '/usr/bin/unsupported-tar',
      ['-tzf', '/build/runtime.tar.gz'],
      { stdio: 'pipe' },
    )
    expect(spawnMock).toHaveBeenNthCalledWith(
      3,
      '/usr/bin/unsupported-tar',
      ['-xzf', '/build/runtime.tar.gz', '-C', '/build/extracted'],
      { stdio: 'ignore' },
    )
  })

  it('probes the exact selected executable for standalone validation', async () => {
    whichMock.mockResolvedValue('/usr/bin/gnu tar')
    tarHelpOutput = '--force-local'

    expect(await validateTarballPaths('/tmp/runtime bundle.tar.gz')).toEqual([
      'runtime/node.exe',
    ])
    expect(spawnMock).toHaveBeenNthCalledWith(
      1,
      '/usr/bin/gnu tar',
      ['--help'],
      {
        killSignal: 'SIGKILL',
        maxBuffer: 64 * 1024,
        stdio: 'pipe',
        timeout: 5000,
      },
    )
    expect(spawnMock).toHaveBeenCalledWith(
      '/usr/bin/gnu tar',
      ['--force-local', '-tzf', '/tmp/runtime bundle.tar.gz'],
      { stdio: 'pipe' },
    )
  })
})
