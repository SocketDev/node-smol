import { afterEach, describe, expect, it } from 'vitest'
import { mkdtemp, mkdir, readFile, rm, writeFile } from 'node:fs/promises'
import os from 'node:os'
import path from 'node:path'
import { spawn } from '@socketsecurity/lib-stable/process/spawn/child'
import { applyNodeSourcePatchChain } from '../../scripts/check-source-patch-chain.mts'

const temporaryRoots: string[] = []

afterEach(async () => {
  await Promise.all(
    temporaryRoots.splice(0).map(root =>
      rm(root, {
        force: true,
        recursive: true,
      }),
    ),
  )
})

async function createPatchSourceFixture() {
  const root = await mkdtemp(path.join(os.tmpdir(), 'node-patch-fixture-'))
  temporaryRoots.push(root)
  const sourcePath = path.join(root, 'source')
  const patchesDir = path.join(root, 'patches')
  const hooksDir = path.join(root, 'hooks')
  await mkdir(sourcePath)
  await mkdir(patchesDir)
  await mkdir(hooksDir)
  await spawn('git', ['init', '--quiet', sourcePath])
  await writeFile(path.join(sourcePath, 'runtime.txt'), 'initial\n')
  await writeFile(path.join(sourcePath, 'config.txt'), 'initial\n')
  await spawn('git', ['add', 'runtime.txt', 'config.txt'], { cwd: sourcePath })
  await spawn(
    'git',
    [
      '-c',
      'user.name=Morgan Example',
      '-c',
      'user.email=morgan@example.com',
      '-c',
      'commit.gpgsign=false',
      '-c',
      `core.hooksPath=${hooksDir}`,
      'commit',
      '--quiet',
      '-m',
      'test: fixture',
    ],
    { cwd: sourcePath },
  )
  const result = await spawn('git', ['rev-parse', 'HEAD'], {
    cwd: sourcePath,
    stdio: 'pipe',
  })
  return { patchesDir, sourceCommit: String(result.stdout).trim(), sourcePath }
}

function createRuntimePatch(
  before: string,
  after: string,
  file = 'runtime.txt',
): string {
  return `--- a/${file}\n+++ b/${file}\n@@ -1 +1 @@\n-${before}\n+${after}\n`
}

describe('Node source patch chain', () => {
  it('applies independent file patches without changing the source checkout', async () => {
    const fixture = await createPatchSourceFixture()
    await writeFile(
      path.join(fixture.patchesDir, '001-first.patch'),
      createRuntimePatch('initial', 'intermediate'),
    )
    await writeFile(
      path.join(fixture.patchesDir, '002-second.patch'),
      createRuntimePatch('initial', 'finished', 'config.txt'),
    )

    expect(await applyNodeSourcePatchChain(fixture)).toBe(2)
    expect(
      await readFile(path.join(fixture.sourcePath, 'runtime.txt'), 'utf8'),
    ).toBe('initial\n')
  })

  it('rejects a patch that does not apply to pristine source', async () => {
    const fixture = await createPatchSourceFixture()
    await writeFile(
      path.join(fixture.patchesDir, '001-first.patch'),
      createRuntimePatch('initial', 'intermediate'),
    )
    await writeFile(
      path.join(fixture.patchesDir, '002-invalid.patch'),
      createRuntimePatch('unrelated', 'finished', 'config.txt'),
    )

    await expect(applyNodeSourcePatchChain(fixture)).rejects.toThrow()
    expect(
      await readFile(path.join(fixture.sourcePath, 'runtime.txt'), 'utf8'),
    ).toBe('initial\n')
  })

  it('rejects multiple patches targeting the same file', async () => {
    const fixture = await createPatchSourceFixture()
    await writeFile(
      path.join(fixture.patchesDir, '001-first.patch'),
      createRuntimePatch('initial', 'intermediate'),
    )
    await writeFile(
      path.join(fixture.patchesDir, '002-second.patch'),
      createRuntimePatch('intermediate', 'finished'),
    )
    await expect(applyNodeSourcePatchChain(fixture)).rejects.toThrow()
  })

  it('rejects a patch targeting multiple files', async () => {
    const fixture = await createPatchSourceFixture()
    await writeFile(
      path.join(fixture.patchesDir, '001-multiple.patch'),
      createRuntimePatch('initial', 'finished') +
        createRuntimePatch('initial', 'finished', 'config.txt'),
    )
    await expect(applyNodeSourcePatchChain(fixture)).rejects.toThrow()
  })

  it('rejects a source checkout at a different commit', async () => {
    const fixture = await createPatchSourceFixture()
    await expect(
      applyNodeSourcePatchChain({
        ...fixture,
        sourceCommit: '0'.repeat(40),
      }),
    ).rejects.toThrow()
  })
})
