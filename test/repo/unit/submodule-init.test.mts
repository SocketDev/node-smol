import { mkdtemp, mkdir, rm, writeFile } from 'node:fs/promises'
import os from 'node:os'
import path from 'node:path'
import process from 'node:process'

import { afterEach, describe, expect, it, vi } from 'vitest'

import { spawn } from '@socketsecurity/lib-stable/process/spawn/child'

import { ensureSubmodule } from '../../../packages/build-infra/lib/submodule-init.mts'

vi.mock('@socketsecurity/lib-stable/process/spawn/child', () => ({
  spawn: vi.fn(),
}))

const directories: string[] = []

afterEach(async () => {
  vi.resetAllMocks()
  await Promise.all(
    directories
      .splice(0)
      .map(directory => rm(directory, { recursive: true, force: true })),
  )
})

async function createSubmoduleFixture() {
  const monorepoRoot = await mkdtemp(path.join(os.tmpdir(), 'submodule-init-'))
  directories.push(monorepoRoot)
  await writeFile(path.join(monorepoRoot, '.git'), 'gitdir: fixture')
  const options = {
    monorepoRoot,
    name: 'fixture-library',
    sentinelFile: 'CMakeLists.txt',
    submodulePath: 'upstream/fixture-library',
  }
  const destination = path.join(monorepoRoot, options.submodulePath)
  return { options, destination }
}

describe('upstream tree initialization', () => {
  it('uses the canonical fleet helper from a worktree', async () => {
    const { options, destination } = await createSubmoduleFixture()
    vi.mocked(spawn).mockImplementationOnce(async () => {
      await mkdir(destination, { recursive: true })
      await writeFile(path.join(destination, options.sentinelFile), '')
      return { code: 0 }
    })

    await ensureSubmodule(options)

    expect(spawn).toHaveBeenCalledWith(
      process.execPath,
      [
        path.join(
          options.monorepoRoot,
          'scripts/fleet/git/submodule/partial.mts',
        ),
        'clone',
        options.submodulePath,
      ],
      { cwd: options.monorepoRoot, stdio: 'inherit' },
    )
  })

  it('does not clone an existing upstream tree', async () => {
    const { options, destination } = await createSubmoduleFixture()
    await mkdir(destination, { recursive: true })
    await writeFile(path.join(destination, options.sentinelFile), '')

    await ensureSubmodule(options)

    expect(spawn).not.toHaveBeenCalled()
  })

  it('preserves the clone failure as the error cause', async () => {
    const { options } = await createSubmoduleFixture()
    const cause = new Error('fixture failure')
    vi.mocked(spawn).mockRejectedValueOnce(cause)

    await expect(ensureSubmodule(options)).rejects.toMatchObject({ cause })
  })
})
