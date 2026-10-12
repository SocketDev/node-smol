import { mkdtemp, readFile, rm, writeFile } from 'node:fs/promises'
import os from 'node:os'
import path from 'node:path'

import { afterEach, describe, expect, it, vi } from 'vitest'

import { createPrebuiltApi } from '../../../packages/build-infra/lib/ensure-prebuilt.mts'
import { RELEASE_ASSETS_MANIFEST_PATH } from '../../../packages/build-infra/lib/paths.mts'

const checksum = vi.hoisted(() => vi.fn())

vi.mock(
  '../../../scripts/fleet/build/infra/lib/release-checksums/core.mts',
  () => ({
    verifyReleaseChecksum: checksum,
  }),
)

vi.mock('@socketsecurity/lib-stable/releases/socket-btm', async () => {
  const fs = await import('node:fs/promises')
  const path = await import('node:path')
  return {
    detectLibc: vi.fn(),
    downloadSocketBtmRelease: vi.fn(
      async (name: string, options: { asset: string; downloadDir: string }) => {
        const archive = path.join(options.downloadDir, options.asset)
        await fs.writeFile(archive, Buffer.from([0x1f, 0x8b]))
        await fs.writeFile(path.join(options.downloadDir, '.version'), name)
        return archive
      },
    ),
  }
})

vi.mock('../../../packages/build-infra/lib/tarball-utils.mts', async () => {
  const fs = await import('node:fs/promises')
  const path = await import('node:path')
  return {
    extractTarball: vi.fn(async (archive: string, destination: string) => {
      await fs.access(archive)
      await fs.writeFile(path.join(destination, 'artifact.bin'), 'valid')
    }),
  }
})

const tempDirs: string[] = []

afterEach(async () => {
  await Promise.all(
    tempDirs.splice(0).map(dir => rm(dir, { recursive: true, force: true })),
  )
  vi.clearAllMocks()
})

async function createApi() {
  const packageRoot = await mkdtemp(path.join(os.tmpdir(), 'ensure-prebuilt-'))
  tempDirs.push(packageRoot)
  return createPrebuiltApi({
    name: 'fixture-builder',
    packageRoot,
    requiredFiles: ['artifact.bin'],
    getCurrentPlatformArch: () => 'darwin-arm64',
    getLocalBuildDir: platformArch =>
      path.join(packageRoot, 'build', platformArch),
  })
}

describe('prebuilt release checksum adapter', () => {
  it('provides pinned checksums for published BoringSSL platforms', async () => {
    const manifest = JSON.parse(
      await readFile(RELEASE_ASSETS_MANIFEST_PATH, 'utf8'),
    )
    expect(manifest.boringssl.tag).toMatch(/^boringssl-\d{8}-[a-f0-9]+$/u)
    const platforms = [
      'darwin-arm64',
      'darwin-x64',
      'linux-arm64',
      'linux-x64',
      'win32-arm64',
      'win32-x64',
    ]
    for (const platform of platforms) {
      expect(
        manifest.boringssl.checksums[`boringssl-${platform}.tar.gz`],
      ).toMatch(/^[a-f0-9]{64}$/u)
    }
  })

  it('uses the member manifest and accepts a valid archive', async () => {
    checksum.mockResolvedValue({
      valid: true,
      expected: 'sha256-expected',
      actual: 'sha256-actual',
    })
    const api = await createApi()

    await expect(api.ensure({ force: true })).resolves.toBe(
      api.getDownloadedDir('darwin-arm64'),
    )
    expect(checksum).toHaveBeenCalledWith({
      manifestPath: RELEASE_ASSETS_MANIFEST_PATH,
      filePath: expect.stringMatching(
        /fixture-builder-darwin-arm64\.tar\.gz$/u,
      ),
      assetName: 'fixture-builder-darwin-arm64.tar.gz',
      tool: 'fixture-builder',
    })
  })

  it('rejects invalid checksums and removes the downloaded archive state', async () => {
    checksum.mockResolvedValue({
      valid: false,
      expected: 'sha256-wanted',
      actual: 'sha256-saw',
    })
    const api = await createApi()

    await expect(api.ensure({ force: true })).rejects.toThrow(Error)
    const directory = api.getDownloadedDir('darwin-arm64')
    await expect(
      readFile(path.join(directory, 'fixture-builder-darwin-arm64.tar.gz')),
    ).rejects.toMatchObject({ code: 'ENOENT' })
    await expect(
      readFile(path.join(directory, '.version')),
    ).rejects.toMatchObject({ code: 'ENOENT' })
  })
})
