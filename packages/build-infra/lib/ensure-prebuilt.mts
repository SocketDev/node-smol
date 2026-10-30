/**
 * Shared factory for builder `ensureX` helpers.
 *
 * Each *-builder package (lief, curl, libpq, dawn, opentui, onnxruntime,
 * yoga-layout, codet5-models, minilm) exposes the same
 * shape of public API:
 *
 * EnsureX(options?)        — local-build → pinned prebuilt download
 * existsAt(dir)            — required-files validation
 * verifyAt(dir)            — same, with the missing-files list
 * getDownloadedDir(arch)   — where downloads land
 * downloadPrebuiltX(opts?) — pulls the manifest-pinned release tarball.
 *
 * Without this factory each builder reimplements ~200 LOC of
 * download/verify/extract logic. The factory centralizes the
 * common path; each builder only declares its name, required-files
 * manifest, and per-platform build-dir resolution.
 *
 * Builder-specific extensions (e.g. LIEF's musl-compatibility check
 * on the extracted lib) are handled via the `onExtracted` hook —
 * factory does NOT bake them in.
 */

import { existsSync, promises as fs, readdirSync } from 'node:fs'
import path from 'node:path'

import { errorMessage } from './error-utils.mts'
// `logTransientErrorHelp` is loaded lazily from inside the catch block
// in downloadPrebuilt() — its transitive `@socketsecurity/lib-stable/
// http-request/convenience` import has an ESM/CJS interop issue that
// fires at module-load time. Lazy-loading defers the resolution to
// the actual error-handling path, where a failure to load it
// degrades gracefully (the transient-error hint is omitted).
import { getDownloadedDir, RELEASE_ASSETS_MANIFEST_PATH } from './paths.mts'
import {
  getEmbeddedChecksum,
  verifyReleaseChecksum,
} from '../../../scripts/fleet/build/infra/lib/release-checksums/core.mts'
import {
  detectLibc,
  downloadSocketBtmRelease,
} from '@socketsecurity/lib-stable/releases/socket-btm'
import { extractTarball } from './tarball-utils.mts'

import { safeDelete, safeMkdir } from '@socketsecurity/lib-stable/fs/safe'
import { getDefaultLogger } from '@socketsecurity/lib-stable/logger/default'

const logger = getDefaultLogger()

export interface PrebuiltConfig {
  /**
   * Builder name as it appears in the gh release tag prefix
   * (e.g. 'lief' → releases tagged `lief-<version-hash>`).
   * Also drives the asset filename: `<name>-<platformArch>.tar.gz`.
   */
  name: string
  /**
   * Path to the builder package's root. The downloaded extract dir
   * lives under `<packageRoot>/build/downloaded/<name>/<platformArch>/`.
   */
  packageRoot: string
  /**
   * Files that must exist, relative to the install dir, for the
   * builder's artifact to be considered complete. Array elements may
   * be strings (single required path) or string arrays (alternatives,
   * any one of which must exist — used for MSVC `.lib` vs Unix `.a`
   * naming variants).
   */
  requiredFiles: Array<string | string[]>
  /**
   * Resolve the current platform-arch identifier (e.g. `darwin-arm64`,
   * `linux-x64-musl`). Per-builder so each can honor its own
   * libc-detection / Windows naming conventions.
   */
  getCurrentPlatformArch: () => string
  /**
   * Resolve the local build directory for a given platform-arch.
   * This is the "if the user already built locally" path checked
   * before trying the pinned prebuilt release download. Returns the
   * directory that should contain `requiredFiles`.
   */
  getLocalBuildDir: (platformArch: string) => string
  /**
   * Optional hook fired after a downloaded tarball is extracted but
   * before the success-return. Lets a builder add platform-specific
   * post-checks (LIEF: verify musl compatibility on linux-*-musl).
   * Return `false` to reject the download (factory will delete the
   * extract dir and return `undefined`, falling through to source
   * build).
   */
  onExtracted?:
    | ((extractDir: string, platformArch: string) => Promise<boolean>)
    | undefined
}

export interface PrebuiltApi {
  /**
   * Validates that every entry in `requiredFiles` resolves under `dir`.
   */
  verifyAt: (dir: string) => { valid: boolean; missing: string[] }
  /**
   * Boolean wrapper around `verifyAt`.
   */
  existsAt: (dir: string) => boolean
  /**
   * `existsAt` over the current platform's local build dir.
   */
  exists: (platformArch?: string | undefined) => boolean
  /**
   * `<packageRoot>/build/downloaded/<name>/<platformArch>/` —
   * where the gh-release download lands after extraction.
   */
  getDownloadedDir: (platformArch: string) => string
  /**
   * Fetch the manifest-pinned release tarball for the given platform.
   * Returns the extract directory on success, `undefined` on failure
   * (network error, checksum mismatch, missing release, onExtracted
   * rejection). Callers fall through to a from-source build.
   */
  downloadPrebuilt: (
    options?:
      | {
          platformArch?: string | undefined
        }
      | undefined,
  ) => Promise<string | undefined>
  /**
   * Orchestrates the lookup chain:
   *
   * 1. Local build — `<getLocalBuildDir(arch)>`
   * 2. Downloaded — `<getDownloadedDir(arch)>`
   * 3. Fetch prebuilt — `downloadPrebuilt({ platformArch: arch })` Returns the
   *    resolved install dir, or throws if none of the three branches produced a
   *    valid install.
   */
  ensure: (
    options?:
      | {
          force?: boolean | undefined
          platformArch?: string | undefined
        }
      | undefined,
  ) => Promise<string>
}

/**
 * Factory entry-point. Each builder calls this once with its config
 * and re-exports the returned API surface from `lib/ensure-X.mts`.
 */
export function createPrebuiltApi(config: PrebuiltConfig): PrebuiltApi {
  const {
    getCurrentPlatformArch,
    getLocalBuildDir,
    name,
    onExtracted,
    packageRoot,
    requiredFiles,
  } = config

  function verifyAt(dir: string): { valid: boolean; missing: string[] } {
    const missing = requiredFiles
      .filter(requirement => {
        if (Array.isArray(requirement)) {
          return !requirement.some(alt => existsSync(path.join(dir, alt)))
        }
        return !existsSync(path.join(dir, requirement))
      })
      .map(req => (Array.isArray(req) ? `{${req.join(',')}}` : req))
    return { valid: missing.length === 0, missing }
  }

  function existsAt(dir: string): boolean {
    return verifyAt(dir).valid
  }

  function getDownloadedDirForArch(platformArch: string): string {
    return path.join(getDownloadedDir(packageRoot), name, platformArch)
  }

  function exists(platformArch?: string | undefined): boolean {
    const resolvedPlatformArch = platformArch ?? getCurrentPlatformArch()
    return existsAt(getLocalBuildDir(resolvedPlatformArch))
  }

  async function deleteArchiveState(
    downloadedArchive: string,
    extractDir: string,
  ): Promise<void> {
    await safeDelete(downloadedArchive)
    const versionFile = path.join(extractDir, '.version')
    if (existsSync(versionFile)) {
      await safeDelete(versionFile)
    }
  }

  async function validateGzipArchive(
    downloadedArchive: string,
    extractDir: string,
  ): Promise<void> {
    const file = await fs.open(downloadedArchive, 'r')
    const magic = Buffer.alloc(2)
    try {
      await file.read(magic, 0, 2, 0)
    } finally {
      await file.close()
    }
    if (magic[0] === 0x1f && magic[1] === 0x8b) {
      return
    }
    await deleteArchiveState(downloadedArchive, extractDir)
    throw new Error(
      'Downloaded archive is not a valid gzip file (missing magic bytes). ' +
        `File may be corrupted or truncated. Deleted ${downloadedArchive} to force re-download.`,
    )
  }

  async function validateArchiveChecksum(
    downloadedArchive: string,
    extractDir: string,
    assetName: string,
  ): Promise<void> {
    logger.info('Verifying archive checksum…')
    const result = await verifyReleaseChecksum({
      manifestPath: RELEASE_ASSETS_MANIFEST_PATH,
      filePath: downloadedArchive,
      assetName,
      tool: name,
    })
    if (!result.valid) {
      await deleteArchiveState(downloadedArchive, extractDir)
      throw new Error(
        'Archive checksum mismatch - file is corrupted.\n' +
          `  Expected: ${result.expected}\n` +
          `  Actual:   ${result.actual}\n` +
          `Deleted ${downloadedArchive} to force re-download.`,
      )
    }
    if (result.actual) {
      logger.info(
        `Checksum verified: ${result.actual.slice(0, 16)}...${result.actual.slice(-8)}`,
      )
    }
  }

  async function cleanExtraction(
    downloadedArchive: string,
    extractDir: string,
  ): Promise<void> {
    const archiveBasename = path.basename(downloadedArchive)
    const items = readdirSync(extractDir)
    for (let i = 0, { length } = items; i < length; i += 1) {
      const item = items[i]!
      if (item === archiveBasename || item === '.version') {
        continue
      }
      await safeDelete(path.join(extractDir, item), {
        maxRetries: 3,
        retryDelay: 100,
      })
    }
  }

  async function extractArchive(
    downloadedArchive: string,
    extractDir: string,
  ): Promise<void> {
    try {
      await extractTarball(downloadedArchive, extractDir, {
        createDir: false,
        stdio: 'inherit',
        validate: true,
      })
    } catch (error) {
      await deleteArchiveState(downloadedArchive, extractDir)
      throw new Error(
        `Failed to extract ${name} archive from ${downloadedArchive}: ${errorMessage(error)}. ` +
          'Deleted corrupted archive to allow re-download on next run.',
        { cause: error },
      )
    }
  }

  async function validateExtractedFiles(
    downloadedArchive: string,
    extractDir: string,
  ): Promise<void> {
    const result = verifyAt(extractDir)
    if (result.valid) {
      return
    }
    await deleteArchiveState(downloadedArchive, extractDir)
    throw new Error(
      `${name} required files missing after extraction in ${extractDir}: ${result.missing.join(', ')}. ` +
        'Deleted cached files to allow re-download on retry.',
    )
  }

  async function downloadPrebuilt(
    options: { platformArch?: string | undefined } = {},
  ): Promise<string | undefined> {
    const { platformArch } = options
    const resolvedPlatformArch = platformArch ?? getCurrentPlatformArch()
    const assetName = `${name}-${resolvedPlatformArch}.tar.gz`
    const targetDir = getDownloadedDirForArch(resolvedPlatformArch)

    try {
      logger.info(`Checking for prebuilt ${name} releases…`)
      const pinnedRelease = getEmbeddedChecksum(name, assetName, {
        manifestPath: RELEASE_ASSETS_MANIFEST_PATH,
      })
      if (!pinnedRelease?.checksum || !pinnedRelease.tag.trim()) {
        throw new Error(
          `No pinned checksum and release tag found for ${name} asset ${assetName}.`,
        )
      }
      await safeMkdir(targetDir)
      logger.info(`Downloading ${assetName}...`)
      const downloadedArchive = await downloadSocketBtmRelease(name, {
        asset: assetName,
        downloadDir: targetDir,
        tag: pinnedRelease.tag,
      })
      const extractDir = path.dirname(downloadedArchive)
      if (!existsSync(downloadedArchive)) {
        throw new Error(
          `Downloaded archive not found at expected path: ${downloadedArchive}`,
        )
      }

      await validateGzipArchive(downloadedArchive, extractDir)
      await validateArchiveChecksum(downloadedArchive, extractDir, assetName)
      await cleanExtraction(downloadedArchive, extractDir)
      await extractArchive(downloadedArchive, extractDir)
      await validateExtractedFiles(downloadedArchive, extractDir)

      // Builder-specific post-check (e.g. LIEF's musl-compat probe).
      if (onExtracted) {
        const accepted = await onExtracted(extractDir, resolvedPlatformArch)
        if (!accepted) {
          await safeDelete(targetDir)
          return undefined
        }
      }

      logger.success(`Successfully downloaded and extracted prebuilt ${name}`)
      return extractDir
    } catch (e) {
      logger.info(`Failed to download prebuilt ${name}: ${errorMessage(e)}`)
      // Lazy-load the transient-error hint so the eager module-load
      // path doesn't depend on @socketsecurity/lib-stable's
      // http-request/convenience (CJS export with ESM-only consumers).
      // If lazy-load fails (e.g. the interop bug bites here too), we
      // skip the hint rather than mask the original error.
      try {
        const { logTransientErrorHelp } =
          await import('./github-error-utils.mts')
        await logTransientErrorHelp(e)
      } catch {
        // Hint module failed to load — original error already logged.
      }
      return undefined
    }
  }

  // Per-platform locks deduplicate concurrent ensure() calls from
  // parallel workers (e.g. multi-arch matrix runs racing on the same
  // downloaded cache). Without this two callers can both miss the
  // local check, both miss the downloaded check, both fetch the
  // prebuilt, and the second's extraction can stomp the first's.
  const locks = new Map<string, Promise<string>>()

  async function ensureImpl(
    resolvedPlatformArch: string,
    // Private closure, not a public API; options-bag is on the outer `ensure`
    // function.
    // oxlint-disable-next-line socket/no-boolean-trap-param -- see note above
    force: boolean,
  ): Promise<string> {
    // 1. Local build first.
    const localDir = getLocalBuildDir(resolvedPlatformArch)
    if (!force && existsAt(localDir)) {
      logger.info(`Using local ${name} at ${localDir}`)
      return localDir
    }

    // 2. Fetch the manifest-pinned prebuilt. The downloader reuses an archive
    // only when its .version matches the pinned release tag.
    logger.info(`${name} not found locally, downloading prebuilt…`)
    const extracted = await downloadPrebuilt({
      platformArch: resolvedPlatformArch,
    })
    if (extracted && existsAt(extracted)) {
      return extracted
    }

    throw new Error(
      `Failed to ensure ${name}. Run \`pnpm --filter ${name}-builder build\` to build from source.`,
    )
  }

  async function ensure(
    options: {
      force?: boolean | undefined
      platformArch?: string | undefined
    } = {},
  ): Promise<string> {
    const { force = false, platformArch } = options
    const resolvedPlatformArch = platformArch ?? getCurrentPlatformArch()

    const existing = locks.get(resolvedPlatformArch)
    if (existing) {
      return existing
    }
    const lockPromise = ensureImpl(resolvedPlatformArch, force)
    locks.set(resolvedPlatformArch, lockPromise)
    try {
      return await lockPromise
    } finally {
      locks.delete(resolvedPlatformArch)
    }
  }

  return {
    downloadPrebuilt,
    ensure,
    exists,
    existsAt,
    getDownloadedDir: getDownloadedDirForArch,
    verifyAt,
  }
}

// Re-export shared utility so consumers can keep one import line.
export { detectLibc }
