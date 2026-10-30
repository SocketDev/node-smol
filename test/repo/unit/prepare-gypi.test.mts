import { mkdir, mkdtemp, readFile, rm, writeFile } from 'node:fs/promises'
import os from 'node:os'
import path from 'node:path'

import { afterEach, describe, expect, it } from 'vitest'

import { generateVendoredGypi } from '../../../packages/node-smol-builder/scripts/binary-released/shared/prepare-gypi.mts'

const tempDirs: string[] = []

afterEach(async () => {
  await Promise.all(
    tempDirs
      .splice(0)
      .map(directory => rm(directory, { recursive: true, force: true })),
  )
})

async function createSourceRoot(): Promise<string> {
  const sourceRoot = await mkdtemp(path.join(os.tmpdir(), 'prepare-gypi-'))
  tempDirs.push(sourceRoot)
  return sourceRoot
}

describe('vendored gypi generation', () => {
  it('fails with a repair command when required lshpack source is absent', async () => {
    const sourceRoot = await createSourceRoot()

    await expect(generateVendoredGypi(sourceRoot)).rejects.toThrow(
      /Required vendored source is missing: deps\/lsquic\/src\/lshpack.*git -C packages\/lsquic-infra\/upstream\/lsquic submodule update/u,
    )
  })

  it('fails when the required lshpack source tree contains no source files', async () => {
    const sourceRoot = await createSourceRoot()
    await mkdir(path.join(sourceRoot, 'deps/lsquic/src/lshpack'), {
      recursive: true,
    })

    await expect(generateVendoredGypi(sourceRoot)).rejects.toThrow(
      /Required vendored source is empty: deps\/lsquic\/src\/lshpack/u,
    )
  })

  it('emits the lshpack source manifest when its source is present', async () => {
    const sourceRoot = await createSourceRoot()
    const lshpackRoot = path.join(sourceRoot, 'deps/lsquic/src/lshpack')
    await mkdir(lshpackRoot, { recursive: true })
    await writeFile(path.join(lshpackRoot, 'lshpack.c'), '')

    await generateVendoredGypi(sourceRoot)

    const gypi = await readFile(
      path.join(sourceRoot, 'deps/lshpack.gypi'),
      'utf8',
    )
    expect(gypi).toContain("'lsquic/src/lshpack/lshpack.c'")
    expect(gypi).toContain("'XXH_INLINE_ALL'")
  })
})
