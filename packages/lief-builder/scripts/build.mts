// max-file-lines: builder-pipeline -- single builder pipeline (fetch → patch → build → package); phases share state variables and early-return across the full sequence — splitting would fracture control flow
/**
 * Build script for LIEF library.
 * Downloads prebuilt LIEF from GitHub releases or builds from source.
 */

import { existsSync, promises as fs, readdirSync } from 'node:fs'
import os from 'node:os'
import path from 'node:path'
import process from 'node:process'

import { fileURLToPath } from 'node:url'

import { checkBuildSourceFlag } from 'local-build-infra/lib/build-env'
import {
  createCheckpoint,
  shouldRun,
} from 'local-build-infra/lib/checkpoint-manager'
import {
  BUILD_STAGES,
  CHECKPOINTS,
  getBuildMode,
  getPlatformBuildDir,
} from 'local-build-infra/lib/constants'
// logTransientErrorHelp loaded lazily inside catch blocks below (see
// curl-builder/scripts/build.mts for full rationale on the
// http-request/convenience CJS/ESM interop crash).
import { appendCCRemapFlags } from 'local-build-infra/lib/path-remap-flags'
import {
  getDownloadedDir,
  RELEASE_ASSETS_MANIFEST_PATH,
} from 'local-build-infra/lib/paths'
import {
  getAssetPlatformArch,
  isMusl,
} from 'local-build-infra/lib/platform-mappings'
import { verifyReleaseChecksum } from '../../../scripts/fleet/build/infra/lib/release-checksums/core.mts'
import { extractTarball } from 'local-build-infra/lib/tarball-utils'
import { getSubmoduleVersion } from 'local-build-infra/lib/tool-versions'
import { errorMessage } from 'local-build-infra/lib/error-utils'

import { which } from '@socketsecurity/lib-stable/exe/path/which'
import { isWin32 } from '@socketsecurity/lib-stable/constants/platform'
import { safeDelete, safeMkdir } from '@socketsecurity/lib-stable/fs/safe'
import { getDefaultLogger } from '@socketsecurity/lib-stable/logger/default'
import {
  detectLibc,
  downloadSocketBtmRelease,
} from '@socketsecurity/lib-stable/releases/socket-btm'
import { spawn } from '@socketsecurity/lib-stable/process/spawn/child'

import { LIEF_REQUIRED_FILES } from '../lib/required-files.mts'
import { getEnvValue } from '@socketsecurity/lib-stable/env/rewire'

const logger = getDefaultLogger()

const __filename = fileURLToPath(import.meta.url)
const __dirname = path.dirname(__filename)
const packageRoot = path.join(__dirname, '..')

const liefUpstream = path.join(packageRoot, 'upstream/lief')
const liefPatchedDir = path.join(
  packageRoot,
  'build',
  'shared',
  'source',
  'lief',
)

/**
 * Get build directories for a given platform-arch.
 *
 * @param {string} platformArch - Platform-arch identifier.
 *
 * @returns {{ buildDir: string; liefBuildDir: string }}
 */
export function getLiefBuildDirs(platformArch) {
  const buildDir = getPlatformBuildDir(packageRoot, platformArch)
  const liefBuildDir = path.join(buildDir, 'out', BUILD_STAGES.FINAL, 'lief')
  return { __proto__: null, buildDir, liefBuildDir }
}

/**
 * Get current platform-arch for LIEF.
 * LIEF releases use 'win' not 'win32' for Windows platforms.
 *
 * @returns {string} Platform-arch identifier.
 */
// Build script is ordered as a top-down pipeline (download → extract →
// configure → build → install → smoke test); alphabetizing across pipeline
// phases would scatter the flow and break the checkpoint reading order.
// oxlint-disable-next-line socket/sort-source-methods -- intentional ordering
export function getCurrentLiefPlatformArch() {
  const libc = detectLibc()
  // Respect TARGET_ARCH for cross-compilation (set by workflows/Makefiles)
  const arch = getEnvValue('TARGET_ARCH') || process.arch
  // Use asset platform naming (win instead of win32).
  return getAssetPlatformArch(process.platform, arch, libc)
}

/**
 * Get the downloaded LIEF directory path.
 * Files are extracted to ${platformArch}/lief/ subdirectory.
 *
 * @param {string} platformArch - Platform-arch identifier.
 *
 * @returns {string} Path to downloaded LIEF directory.
 */
// Build script is ordered as a top-down pipeline (download → extract →
// configure → build → install → smoke test); alphabetizing across pipeline
// phases would scatter the flow and break the checkpoint reading order.
// oxlint-disable-next-line socket/sort-source-methods -- intentional ordering
export function getDownloadedLiefDir(platformArch) {
  return path.join(getDownloadedDir(packageRoot), 'lief', platformArch)
}

/**
 * Verify LIEF archive integrity using SHA256 checksum.
 * Downloads checksums.txt from the release dynamically using shared utility.
 *
 * @param {string} archivePath - Path to archive file.
 * @param {string} assetName - Asset name for checksum lookup.
 *
 * @returns {Promise<{
 *   valid: boolean
 *   expected?: string
 *   actual?: string
 *   skipped?: boolean
 * }>}
 */
export async function verifyArchiveChecksum(archivePath, assetName) {
  return verifyReleaseChecksum({
    manifestPath: RELEASE_ASSETS_MANIFEST_PATH,
    filePath: archivePath,
    assetName,
    tool: 'lief',
  })
}

/**
 * Get required LIEF files for validation.
 *
 * @returns {(string | string[])[]} Array of required files. Arrays indicate
 *   alternatives, of which any one must exist.
 */
// Build script is ordered as a top-down pipeline (download → extract →
// configure → build → install → smoke test); alphabetizing across pipeline
// phases would scatter the flow and break the checkpoint reading order.
// oxlint-disable-next-line socket/sort-source-methods -- intentional ordering
export function getLiefRequiredFiles() {
  return LIEF_REQUIRED_FILES
}

/**
 * Verify all required LIEF files exist at a directory.
 *
 * @param {string} dir - Directory to check.
 *
 * @returns {{ valid: boolean; missing: string[] }} Validation result with list
 *   of missing files.
 */
export function verifyLiefAt(dir) {
  const requiredFiles = getLiefRequiredFiles()
  const missing = requiredFiles
    .filter(requirement => {
      // Array means alternatives - any one must exist
      if (Array.isArray(requirement)) {
        return !requirement.some(alt => existsSync(path.join(dir, alt)))
      }
      return !existsSync(path.join(dir, requirement))
    })
    .map(req => (Array.isArray(req) ? `{${req.join(',')}}` : req))
  return {
    __proto__: null,
    valid: missing.length === 0,
    missing,
  }
}

/**
 * Check if LIEF installation is complete at a given directory.
 * Validates library file AND all required headers exist.
 *
 * @param {string} dir - Directory to check.
 *
 * @returns {boolean} True if complete LIEF installation exists.
 */
// Build script is ordered as a top-down pipeline (download → extract →
// configure → build → install → smoke test); alphabetizing across pipeline
// phases would scatter the flow and break the checkpoint reading order.
// oxlint-disable-next-line socket/sort-source-methods -- intentional ordering
export function liefExistsAt(dir) {
  return verifyLiefAt(dir).valid
}

/**
 * Get the LIEF library path at a specific directory (platform-specific).
 *
 * @param {string} dir - Directory to check.
 *
 * @returns {string | undefined} Path to LIEF library if exists, undefined
 *   otherwise.
 */
// Build script is ordered as a top-down pipeline (download → extract →
// configure → build → install → smoke test); alphabetizing across pipeline
// phases would scatter the flow and break the checkpoint reading order.
// oxlint-disable-next-line socket/sort-source-methods -- intentional ordering
export function getLiefLibPathAt(dir) {
  const unixPath = path.join(dir, 'libLIEF.a')
  const msvcPath = path.join(dir, 'LIEF.lib')

  if (existsSync(unixPath)) {
    return unixPath
  }
  if (existsSync(msvcPath)) {
    return msvcPath
  }
  return undefined
}

/**
 * Get the LIEF library path (platform-specific).
 *
 * @param {string} [platformArch] - Platform-arch identifier. Defaults to
 *   current platform.
 *
 * @returns {string | undefined} Path to LIEF library if exists, undefined
 *   otherwise.
 */
// Build script is ordered as a top-down pipeline (download → extract →
// configure → build → install → smoke test); alphabetizing across pipeline
// phases would scatter the flow and break the checkpoint reading order.
// oxlint-disable-next-line socket/sort-source-methods -- intentional ordering
export function getLiefLibPath(platformArch) {
  const resolvedPlatformArch = platformArch ?? getCurrentLiefPlatformArch()
  const { liefBuildDir } = getLiefBuildDirs(resolvedPlatformArch)
  return getLiefLibPathAt(liefBuildDir)
}

/**
 * Check if LIEF library exists.
 *
 * @param {string} [platformArch] - Platform-arch identifier. Defaults to
 *   current platform.
 *
 * @returns {boolean} True if LIEF library exists.
 */
// Build script is ordered as a top-down pipeline (download → extract →
// configure → build → install → smoke test); alphabetizing across pipeline
// phases would scatter the flow and break the checkpoint reading order.
// oxlint-disable-next-line socket/sort-source-methods -- intentional ordering
export function liefExists(platformArch) {
  return getLiefLibPath(platformArch) !== undefined
}

/**
 * Extract LIEF version from .gitmodules comment.
 * The version is specified in the comment above the LIEF submodule entry.
 *
 * @returns {string} LIEF version (e.g., "0.17.0")
 */
// Build script is ordered as a top-down pipeline (download → extract →
// configure → build → install → smoke test); alphabetizing across pipeline
// phases would scatter the flow and break the checkpoint reading order.
// oxlint-disable-next-line socket/sort-source-methods -- intentional ordering
export function getLiefVersion() {
  const version = getSubmoduleVersion(
    'packages/lief-builder/upstream/lief',
    'lief',
  )
  logger.info(`Detected LIEF version from .gitmodules: ${version}`)
  return version
}

// LIEF version (extracted from .gitmodules comment).
const LIEF_VERSION = getLiefVersion()

// Build script is ordered as a top-down pipeline (download → extract →
// configure → build → install → smoke test); alphabetizing across pipeline
// phases would scatter the flow and break the checkpoint reading order.
// oxlint-disable-next-line socket/sort-source-methods -- intentional ordering
export async function runCommand(command, args, cwd, env = {}) {
  logger.info(`Running: ${command} ${args.join(' ')}`)

  // Merge env properly, filtering out undefined values.
  const mergedEnv = { ...process.env }
  // Loop variable is destructured.
  // oxlint-disable-next-line socket/prefer-cached-for-loop -- see above
  for (const [key, value] of Object.entries(env)) {
    if (value === undefined) {
      delete mergedEnv[key]
    } else {
      mergedEnv[key] = value
    }
  }

  const result = await spawn(command, args, {
    cwd,
    env: mergedEnv,
    stdio: 'inherit',
  })

  if (result.error) {
    throw new Error(`Command failed to spawn: ${errorMessage(result.error)}`)
  }

  if (result.signal) {
    throw new Error(`Command terminated by signal: ${result.signal}`)
  }

  if (result.code !== 0) {
    throw new Error(`Command failed with exit code ${result.code}`)
  }
}

/**
 * Run robocopy for the Windows source-copy path.
 *
 * @param {string} sourceDir - Destination directory for copied source.
 */
// Build script is ordered as a top-down pipeline (download → extract → configure → build → install → smoke test); alphabetizing across pipeline phases would scatter the flow and break the checkpoint reading order.
// oxlint-disable-next-line socket/sort-source-methods -- intentional ordering
export async function runLiefRobocopy(sourceDir) {
  logger.info('Running: robocopy (copying LIEF source)')
  let result
  try {
    result = await spawn(
      'robocopy',
      [
        liefUpstream,
        sourceDir,
        '/E',
        '/XD',
        '.git',
        '/NFL',
        '/NDL',
        '/NJH',
        '/NJS',
      ],
      { cwd: packageRoot, stdio: 'inherit' },
    )
  } catch (e) {
    if (typeof e?.code === 'string') {
      throw new Error(`robocopy did not run: ${e.code} (${errorMessage(e)})`)
    }
    if (e?.code >= 8) {
      throw new Error(
        `robocopy failed with exit code ${e.code} (indicates error)`,
      )
    }
    result = e
  }

  if (result.error) {
    throw new Error(`robocopy failed to spawn: ${errorMessage(result.error)}`)
  }
  if (result.signal) {
    throw new Error(`robocopy terminated by signal: ${result.signal}`)
  }
  return result
}

/**
 * Verify the Windows LIEF source copy is present, readable, and nonempty.
 *
 * @param {string} sourceDir - Destination directory for copied source.
 */
// Build script is ordered as a top-down pipeline (download → extract → configure → build → install → smoke test); alphabetizing across pipeline phases would scatter the flow and break the checkpoint reading order.
// oxlint-disable-next-line socket/sort-source-methods -- intentional ordering
export function verifyCopiedLiefSource(sourceDir) {
  if (!existsSync(sourceDir)) {
    throw new Error('robocopy completed but source directory is missing')
  }
  try {
    if (readdirSync(sourceDir).length === 0) {
      throw new Error('robocopy completed but source directory is empty')
    }
  } catch (e) {
    if (e?.code === 'EACCES' || e?.code === 'EPERM') {
      throw new Error(
        `robocopy completed but cannot read source directory: ${errorMessage(e)}`,
      )
    }
    throw e
  }
}

/**
 * Verify that a prebuilt LIEF library is compatible with musl libc.
 * Checks for glibc-specific fortify symbols that would cause linker errors.
 *
 * @param {string} libPath - Path to libLIEF.a.
 *
 * @returns {Promise<{ compatible: boolean; reason?: string }>}
 */
export async function verifyMuslCompatibility(libPath) {
  if (!(await isMusl())) {
    return { __proto__: null, compatible: true }
  }

  logger.info('Verifying LIEF library for musl compatibility…')

  // Use nm to check for glibc-specific fortify symbols. These (__memcpy_chk,
  // __printf_chk, …) don't exist in musl. The check must default-deny:
  // missing nm, nm error, or empty stdout all mean "cannot verify", which
  // is the exact failure mode this gate exists to catch.

  // First check if nm is available.
  try {
    await spawn('which', ['nm'], { stdio: 'pipe' })
  } catch {
    return {
      __proto__: null,
      compatible: false,
      reason: 'nm not found; cannot verify musl compatibility',
    }
  }

  // Run nm and capture output.
  let nmOutput
  try {
    const result = await spawn('nm', [libPath], { stdio: 'pipe' })
    nmOutput = result?.stdout
  } catch (nmError) {
    return {
      __proto__: null,
      compatible: false,
      reason: `nm failed on ${libPath}: ${errorMessage(nmError)}`,
    }
  }

  if (!nmOutput) {
    return {
      __proto__: null,
      compatible: false,
      reason: 'nm returned empty output; cannot verify musl compatibility',
    }
  }

  // Check for common glibc fortify symbols.
  const glibcSymbols = [
    '__memcpy_chk',
    '__memmove_chk',
    '__memset_chk',
    '__strcpy_chk',
    '__strncpy_chk',
    '__strcat_chk',
    '__strncat_chk',
    '__sprintf_chk',
    '__snprintf_chk',
    '__printf_chk',
    '__fprintf_chk',
    '__vprintf_chk',
    '__vfprintf_chk',
    '__vsprintf_chk',
    '__vsnprintf_chk',
  ]

  const foundSymbols = glibcSymbols.filter(sym => nmOutput.includes(sym))

  if (foundSymbols.length > 0) {
    logger.info(`Found ${foundSymbols.length} glibc fortify symbol(s)`)
    return {
      __proto__: null,
      compatible: false,
      reason: `Library contains glibc-specific fortify symbols: ${foundSymbols.join(', ')}`,
    }
  }

  logger.info('No glibc fortify symbols found - library is musl-compatible')
  return { __proto__: null, compatible: true }
}

/**
 * Copy LIEF source from upstream to build directory.
 * This allows patching without modifying the git submodule.
 *
 * @param {string} sourceDir - Destination directory for copied source.
 */
// Build script is ordered as a top-down pipeline (download → extract →
// configure → build → install → smoke test); alphabetizing across pipeline
// phases would scatter the flow and break the checkpoint reading order.
// oxlint-disable-next-line socket/sort-source-methods -- intentional ordering
export async function copyLiefSource(sourceDir) {
  if (!existsSync(liefUpstream)) {
    throw new Error(
      `LIEF upstream not found at ${liefUpstream}. Run 'node scripts/fleet/git-partial-submodule.mts clone packages/lief-builder/upstream/lief' first.`,
    )
  }

  if (existsSync(sourceDir)) {
    await safeDelete(sourceDir)
  }

  await safeMkdir(path.dirname(sourceDir))

  logger.info('Copying LIEF source to build directory…')
  if (isWin32()) {
    const result = await runLiefRobocopy(sourceDir)
    verifyCopiedLiefSource(sourceDir)
    logger.info(`robocopy completed with exit code ${result.code} (success)`)
  } else {
    // Unix: use rsync or cp
    try {
      await runCommand(
        'rsync',
        ['-a', '--exclude=.git', `${liefUpstream}/`, sourceDir],
        packageRoot,
      )
    } catch {
      // Fallback to cp if rsync not available
      await runCommand('cp', ['-r', liefUpstream, sourceDir], packageRoot)
      // Remove .git if it was copied
      const gitDir = path.join(sourceDir, '.git')
      if (existsSync(gitDir)) {
        await safeDelete(gitDir)
      }
    }
  }

  logger.success('LIEF source copied')
}

/**
 * Apply Socket patches to LIEF source.
 * Patches are applied in order from patches/lief/*.patch
 * Uses `patch -p1` command (doesn't require git).
 *
 * @param {string} sourceDir - Path to LIEF source directory.
 */
// Build script is ordered as a top-down pipeline (download → extract →
// configure → build → install → smoke test); alphabetizing across pipeline
// phases would scatter the flow and break the checkpoint reading order.
// oxlint-disable-next-line socket/sort-source-methods -- intentional ordering
export async function applyLiefPatches(sourceDir) {
  const patchesDir = path.join(packageRoot, 'patches', 'lief')

  if (!existsSync(patchesDir)) {
    logger.info('No LIEF patches directory found, skipping patch application')
    return
  }

  const patches = readdirSync(patchesDir)
    .filter(f => f.endsWith('.patch'))
    .toSorted()

  if (patches.length === 0) {
    logger.info('No LIEF patches found')
    return
  }

  logger.info(`Applying ${patches.length} LIEF patch(es)...`)

  for (let i = 0, { length } = patches; i < length; i += 1) {
    const patchFile = patches[i]
    const patchPath = path.join(patchesDir, patchFile)
    logger.info(`  Applying ${patchFile}...`)

    try {
      // Check if patch is already applied (--dry-run with -R checks reverse)
      const checkResult = await spawn(
        'patch',
        ['-p1', '--dry-run', '-R', '-i', patchPath],
        { cwd: sourceDir, stdio: 'pipe' },
      )
      if (checkResult.code === 0) {
        logger.info('    Already applied, skipping')
        continue
      }
    } catch {
      // Patch not applied, continue to apply it
    }

    try {
      // Apply the patch using patch -p1 (doesn't require git)
      await runCommand('patch', ['-p1', '-i', patchPath], sourceDir)
      logger.info('    Applied successfully')
    } catch (e) {
      throw new Error(
        `Failed to apply patch ${patchFile}: ${errorMessage(e)}`,
        {
          cause: e,
        },
      )
    }
  }

  logger.success('All LIEF patches applied')
}

/**
 * Delete a downloaded archive and its version marker.
 *
 * @param {string} downloadedArchive - Path to the downloaded archive.
 * @param {string} extractDir - Directory containing the archive.
 */
// Build script is ordered as a top-down pipeline (download → extract →
// configure → build → install → smoke test); alphabetizing across pipeline
// phases would scatter the flow and break the checkpoint reading order.
// oxlint-disable-next-line socket/sort-source-methods -- intentional ordering
export async function deleteLiefArchiveState(downloadedArchive, extractDir) {
  const versionFile = path.join(extractDir, '.version')
  await safeDelete(downloadedArchive)
  if (existsSync(versionFile)) {
    await safeDelete(versionFile)
  }
}

/**
 * Verify the downloaded LIEF archive before extraction.
 *
 * @param {string} downloadedArchive - Path to the downloaded archive.
 * @param {string} assetName - Release asset name.
 * @param {string} extractDir - Directory containing the archive.
 */
// Build script is ordered as a top-down pipeline (download → extract → configure → build → install → smoke test); alphabetizing across pipeline phases would scatter the flow and break the checkpoint reading order.
// oxlint-disable-next-line socket/sort-source-methods -- intentional ordering
export async function verifyDownloadedLiefArchive(
  downloadedArchive,
  assetName,
  extractDir,
) {
  if (!existsSync(downloadedArchive)) {
    throw new Error(
      `Downloaded archive not found at expected path: ${downloadedArchive}`,
    )
  }

  // Multiple fs.stat() calls consume stats.size for downloaded-archive /
  // built-library size reporting and minimum-size quick checks.
  // oxlint-disable-next-line socket/prefer-exists-sync -- see above
  const archiveStats = await fs.stat(downloadedArchive)
  logger.info(
    `Archive size: ${(archiveStats.size / 1024 / 1024).toFixed(2)} MB`,
  )

  const gzipMagic = Buffer.alloc(2)
  const fd = await fs.open(downloadedArchive, 'r')
  try {
    await fd.read(gzipMagic, 0, 2, 0)
  } finally {
    await fd.close()
  }

  if (gzipMagic[0] !== 0x1f || gzipMagic[1] !== 0x8b) {
    await deleteLiefArchiveState(downloadedArchive, extractDir)
    throw new Error(
      'Downloaded archive is not a valid gzip file (missing magic bytes). ' +
        `File may be corrupted or truncated. Deleted ${downloadedArchive} to force re-download.`,
    )
  }

  logger.info('Verifying archive checksum…')
  const checksumResult = await verifyArchiveChecksum(
    downloadedArchive,
    assetName,
  )
  if (!checksumResult.valid) {
    await deleteLiefArchiveState(downloadedArchive, extractDir)
    throw new Error(
      'Archive checksum mismatch - file is corrupted.\n' +
        `  Expected: ${checksumResult.expected}\n` +
        `  Actual:   ${checksumResult.actual}\n` +
        `Deleted ${downloadedArchive} to force re-download.`,
    )
  }
  if (checksumResult.actual) {
    logger.info(
      `Checksum verified: ${checksumResult.actual.slice(0, 16)}...${checksumResult.actual.slice(-8)}`,
    )
  }

  const EXPECTED_SIZES = {
    __proto__: null,
  }
  const expectedSize = EXPECTED_SIZES[assetName]
  if (expectedSize && archiveStats.size !== expectedSize) {
    await deleteLiefArchiveState(downloadedArchive, extractDir)
    throw new Error(
      'Archive size mismatch - file may be incomplete.\n' +
        `  Expected: ${expectedSize} bytes\n` +
        `  Actual:   ${archiveStats.size} bytes\n` +
        `Deleted ${downloadedArchive} to force re-download.`,
    )
  }
}

/**
 * Download prebuilt LIEF from GitHub releases.
 *
 * @param {object} [options] - Download options.
 * @param {string} [options.platformArch] - Override platform-arch.
 *
 * @returns {Promise<string | null>} Path to downloaded LIEF directory, or null
 *   on failure.
 */
// Build script is ordered as a top-down pipeline (download → extract → configure → build → install → smoke test); alphabetizing across pipeline phases would scatter the flow and break the checkpoint reading order.
// oxlint-disable-next-line socket/sort-source-methods -- intentional ordering
export async function downloadPrebuiltLIEF(options = {}) {
  // Check if download is blocked by BUILD_DEPS_FROM_SOURCE environment flag.
  checkBuildSourceFlag('LIEF', 'DEPS', {
    buildCommand: 'Install LIEF system-wide or build from source',
  })

  const { platformArch } = options
  const resolvedPlatformArch = platformArch ?? getCurrentLiefPlatformArch()

  try {
    logger.info('Checking for prebuilt LIEF releases…')

    const assetName = `lief-${resolvedPlatformArch}.tar.gz`
    const targetDir = path.join(
      packageRoot,
      'build',
      'downloaded',
      'lief',
      resolvedPlatformArch,
    )

    // Create download directory.
    await safeMkdir(targetDir)

    // Download archive using socket-btm release helper.
    logger.info(`Downloading ${assetName}...`)

    const downloadedArchive = await downloadSocketBtmRelease('lief', {
      asset: assetName,
      downloadDir: targetDir,
    })

    // Extract archive to the same directory as the downloaded archive
    const extractDir = path.dirname(downloadedArchive)
    logger.info('Extracting LIEF archive…')

    await verifyDownloadedLiefArchive(downloadedArchive, assetName, extractDir)

    // Clean extraction directory to prevent "File exists" errors from cached layers.
    // This handles cases where Docker cached a partial extraction before failure,
    // or when downloadSocketBtmRelease returns a cached archive with stale extraction.
    // Delete ALL contents except the archive itself to ensure clean extraction.
    // Use safeDelete with retry logic to handle transient filesystem errors.
    const extractDirContents = readdirSync(extractDir)
    const archiveBasename = path.basename(downloadedArchive)
    for (let i = 0, { length } = extractDirContents; i < length; i += 1) {
      const item = extractDirContents[i]
      // Keep the archive and version file, delete everything else
      if (item !== archiveBasename && item !== '.version') {
        const itemPath = path.join(extractDir, item)
        await safeDelete(itemPath, { maxRetries: 3, retryDelay: 100 })
      }
    }

    // Extract using cross-platform tarball utility (handles Windows path conversion).
    // Release tarballs are already flat - no top-level directory to strip.
    try {
      await extractTarball(downloadedArchive, extractDir, {
        createDir: false,
        stdio: 'inherit',
        validate: true,
      })
    } catch (e) {
      // On extraction failure, delete the corrupted archive to allow re-download.
      await deleteLiefArchiveState(downloadedArchive, extractDir)
      throw new Error(
        `Failed to extract LIEF archive from ${downloadedArchive}: ${errorMessage(e)}. ` +
          'Deleted corrupted archive to allow re-download on next run.',
        { cause: e },
      )
    }

    // Verify library file exists after extraction.
    const extractedLibPath = getLiefLibPathAt(extractDir)
    if (!extractedLibPath) {
      let dirContents
      try {
        dirContents = readdirSync(extractDir)
      } catch {
        dirContents = ['<unable to read directory>']
      }

      // Library missing after extraction - delete cached files to force re-download on retry.
      // This handles corrupted/incomplete cached tarballs.
      await deleteLiefArchiveState(downloadedArchive, extractDir)

      throw new Error(
        `LIEF library not found after extraction in ${extractDir}. ` +
          `Directory contains: ${dirContents.join(', ')}. ` +
          `Deleted cached files to allow re-download on retry.`,
      )
    }

    // Verify the downloaded library is compatible with musl if running on musl.
    if (extractedLibPath.endsWith('libLIEF.a')) {
      const compatibility = await verifyMuslCompatibility(extractedLibPath)
      if (!compatibility.compatible) {
        logger.info(
          `Prebuilt LIEF is not compatible with musl: ${compatibility.reason || 'Unknown reason'}`,
        )
        logger.info('Will need to build from source for musl compatibility')
        // Clean up the incompatible download.
        await safeDelete(targetDir)
        return undefined
      }
    }

    logger.success('Successfully downloaded and extracted prebuilt LIEF')
    return extractDir
  } catch (e) {
    logger.info(`Failed to download prebuilt LIEF: ${errorMessage(e)}`)
    try {
      const { logTransientErrorHelp } =
        await import('local-build-infra/lib/github-error-utils')
      await logTransientErrorHelp(e)
    } catch {
      // Hint module failed to load — original error already logged.
    }
    return undefined
  }
}

// Lock to prevent concurrent LIEF downloads/extractions.
// Key is platformArch, value is promise that resolves when extraction is complete.
const ensureLiefLocks = new Map()

/**
 * Ensure LIEF library is available.
 * Checks local build first, then downloaded, then downloads if needed.
 * Uses a lock to prevent concurrent downloads for the same platform.
 *
 * @param {object} [options] - Options.
 * @param {boolean} [options.force] - Force redownload even if LIEF exists.
 * @param {string} [options.buildMode] - Override build mode.
 * @param {string} [options.platformArch] - Override platform-arch for
 *   downloads.
 *
 * @returns {Promise<string>} Path to LIEF library.
 */
// Build script is ordered as a top-down pipeline (download → extract →
// configure → build → install → smoke test); alphabetizing across pipeline
// phases would scatter the flow and break the checkpoint reading order.
// oxlint-disable-next-line socket/sort-source-methods -- intentional ordering
export async function ensureLief(options = {}) {
  const resolvedPlatformArch =
    options.platformArch ?? getCurrentLiefPlatformArch()

  // Check if another ensureLief call is already in progress for this platform.
  const existingLock = ensureLiefLocks.get(resolvedPlatformArch)
  if (existingLock) {
    logger.info(
      `Waiting for concurrent LIEF download for ${resolvedPlatformArch}...`,
    )
    return existingLock
  }

  // Create a new lock for this platform.
  const lockPromise = ensureLiefImpl(options)
  ensureLiefLocks.set(resolvedPlatformArch, lockPromise)

  try {
    return await lockPromise
  } finally {
    // Release the lock when done.
    ensureLiefLocks.delete(resolvedPlatformArch)
  }
}

/**
 * Internal implementation of ensureLief.
 */
// Build script is ordered as a top-down pipeline (download → extract →
// configure → build → install → smoke test); alphabetizing across pipeline
// phases would scatter the flow and break the checkpoint reading order.
// oxlint-disable-next-line socket/sort-source-methods -- intentional ordering
export async function ensureLiefImpl(options = {}) {
  const { force = false, platformArch } = options
  const resolvedPlatformArch = platformArch ?? getCurrentLiefPlatformArch()

  // 1. Check local build first (platform-specific directory).
  const { liefBuildDir } = getLiefBuildDirs(resolvedPlatformArch)
  if (!force && liefExistsAt(liefBuildDir)) {
    const localLibPath = getLiefLibPathAt(liefBuildDir)
    logger.info(`Using local LIEF at ${localLibPath}`)
    return localLibPath
  }

  // 2. Check downloaded version.
  const downloadedDir = getDownloadedLiefDir(resolvedPlatformArch)
  if (!force && liefExistsAt(downloadedDir)) {
    const downloadedLibPath = getLiefLibPathAt(downloadedDir)
    logger.info(`Using downloaded LIEF at ${downloadedLibPath}`)
    return downloadedLibPath
  }

  // 3. Download prebuilt LIEF.
  logger.info('LIEF not found locally, downloading prebuilt…')
  const downloadDir = await downloadPrebuiltLIEF({
    platformArch: resolvedPlatformArch,
  })
  if (downloadDir && liefExistsAt(downloadDir)) {
    const newLibPath = getLiefLibPathAt(downloadDir)
    return newLibPath
  }

  throw new Error(
    'Failed to ensure LIEF. Run: node scripts/fleet/git-partial-submodule.mts clone packages/lief-builder/upstream/lief',
  )
}

/**
 * Download prebuilt LIEF and write its build checkpoint when validation passes.
 *
 * @param {object} options - Prebuilt checkpoint options.
 * @param {string} options.platformArch - Target platform and architecture.
 * @param {string} options.buildDir - Platform build directory.
 *
 * @returns {Promise<boolean>} True when the prebuilt checkpoint is complete.
 */
// Build script is ordered as a top-down pipeline (download → extract → configure → build → install → smoke test); alphabetizing across pipeline phases would scatter the flow and break the checkpoint reading order.
// oxlint-disable-next-line socket/sort-source-methods -- intentional ordering
export async function checkpointPrebuiltLief({ platformArch, buildDir }) {
  const downloadDir = await downloadPrebuiltLIEF({ platformArch })
  if (!downloadDir) {
    return false
  }
  const liefLibPathNew = getLiefLibPathAt(downloadDir)
  if (!liefLibPathNew) {
    return false
  }

  // Multiple fs.stat() calls consume stats.size for downloaded-archive /
  // built-library size reporting and minimum-size quick checks.
  // oxlint-disable-next-line socket/prefer-exists-sync -- see above
  const stats = await fs.stat(liefLibPathNew)
  const sizeMB = (stats.size / 1024 / 1024).toFixed(2)
  const targetArch = getEnvValue('TARGET_ARCH') || process.arch
  const targetPlatform = isWin32() ? 'win' : process.platform

  await createCheckpoint(
    buildDir,
    CHECKPOINTS.LIEF_BUILT,
    async () => {
      // Verify library exists and has reasonable size.
      // oxlint-disable-next-line socket/prefer-exists-sync -- see above
      const libStats = await fs.stat(liefLibPathNew)
      if (libStats.size < 1_000_000) {
        throw new Error(
          `LIEF library too small: ${libStats.size} bytes (expected >1MB)`,
        )
      }

      // Verify all required files exist.
      logger.info(`Verifying LIEF at: ${downloadDir}`)
      const verification = verifyLiefAt(downloadDir)
      if (!verification.valid) {
        logger.error(`LIEF verification failed!`)
        logger.error(`Checked directory: ${downloadDir}`)
        logger.error(`Missing files: ${verification.missing.join(', ')}`)
        try {
          const actualContents = readdirSync(downloadDir)
          logger.error(`Actual contents: ${actualContents.join(', ')}`)
          if (actualContents.includes('include')) {
            const includeContents = readdirSync(
              path.join(downloadDir, 'include'),
            )
            logger.error(`Contents of include/: ${includeContents.join(', ')}`)
          }
        } catch (e) {
          logger.error(`Failed to list directory contents: ${errorMessage(e)}`)
        }
        throw new Error(
          `Incomplete LIEF download - missing files:\n  ${verification.missing.join('\n  ')}`,
        )
      }
    },
    {
      arch: targetArch,
      artifactPath: downloadDir,
      buildDir: path.relative(packageRoot, downloadDir),
      libPath: path.relative(buildDir, liefLibPathNew),
      libSize: stats.size,
      libSizeMB: sizeMB,
      platform: targetPlatform,
      platformArch,
      version: LIEF_VERSION,
    },
  )
  return true
}

/**
 * Append Windows compiler and generator options to the LIEF CMake arguments.
 *
 * @param {string[]} cmakeArgs - CMake arguments to update.
 */
// Build script is ordered as a top-down pipeline (download → extract → configure → build → install → smoke test); alphabetizing across pipeline phases would scatter the flow and break the checkpoint reading order.
// oxlint-disable-next-line socket/sort-source-methods -- intentional ordering
export function appendWindowsLiefCmakeArgs(cmakeArgs) {
  if (!isWin32()) {
    return
  }
  const targetArch = getEnvValue('TARGET_ARCH')
  const isCrossCompileArm64 = targetArch === 'aarch64' || targetArch === 'arm64'
  const cc = isCrossCompileArm64 ? 'aarch64-w64-mingw32-gcc' : 'gcc'
  const cxx = isCrossCompileArm64 ? 'aarch64-w64-mingw32-g++' : 'g++'
  cmakeArgs.push(`-DCMAKE_C_COMPILER=${cc}`, `-DCMAKE_CXX_COMPILER=${cxx}`)
  cmakeArgs.push('-G', 'MinGW Makefiles')
  if (isCrossCompileArm64) {
    cmakeArgs.push('-DCMAKE_SYSTEM_NAME=Windows')
    cmakeArgs.push('-DCMAKE_SYSTEM_PROCESSOR=aarch64')
    logger.info(
      'Building LIEF with aarch64-w64-mingw32-gcc for ARM64 cross-compilation',
    )
  } else {
    logger.info('Building LIEF with gcc/g++ using MinGW Makefiles')
  }
}

/**
 * Configure LIEF with its platform-specific CMake options and clean
 * environment.
 *
 * @param {string} liefBuildDir - LIEF build directory.
 *
 * @returns {Promise<{ cleanEnv: object; muslLibc: boolean }>}
 */
// Build script is ordered as a top-down pipeline (download → extract → configure → build → install → smoke test); alphabetizing across pipeline phases would scatter the flow and break the checkpoint reading order.
// oxlint-disable-next-line socket/sort-source-methods -- intentional ordering
export async function configureLiefBuild(liefBuildDir) {
  logger.info('Configuring LIEF with CMake…')
  const cmakeArgs = [
    liefPatchedDir,
    '-DCMAKE_BUILD_TYPE=Release',
    '-DLIEF_PYTHON_API=OFF',
    '-DLIEF_C_API=OFF',
    '-DLIEF_EXAMPLES=OFF',
    '-DLIEF_TESTS=OFF',
    '-DLIEF_DOC=OFF',
    '-DLIEF_LOGGING=OFF',
    '-DLIEF_LOGGING_DEBUG=OFF',
    '-DLIEF_ENABLE_JSON=OFF',
    // Drop Android-format support — smol-Node only needs ELF (Linux),
    // PE (Windows), MachO (macOS). DEX/ART are Android bytecode/AOT
    // formats; the only consumer is the SEA-binject pipeline which
    // doesn't touch Android. ~30% smaller libLIEF.a.
    '-DLIEF_DEX=OFF',
    '-DLIEF_ART=OFF',
  ]

  // Enable LTO (Link-Time Optimization) for Linux glibc builds.
  // This produces LTO bytecode in libLIEF.a that consumers can use with
  // -flto. GCC LTO bytecode is only readable by the SAME GCC major, so the
  // archive must ALSO carry real machine code (-ffat-lto-objects) for
  // consumers on a different GCC: a slim-LTO archive hard-fails their link
  // with "bytecode stream ... generated with GCC compiler older than 10.0"
  // (ubuntu-24.04 GCC 13 linking binject/binpress against an older-
  // toolchain artifact) and, being bytecode-only (.text size 0), offers no
  // -fno-lto fallback. Fat objects keep cross-module LTO for matched-GCC
  // consumers (the btm-builder gcc-toolset container) and give mismatched
  // consumers real code to link. Passed via CMAKE_*_FLAGS instead of
  // -DCMAKE_INTERPROCEDURAL_OPTIMIZATION=ON because CMake's IPO support
  // appends -fno-fat-lto-objects AFTER user flags on GCC, which would win
  // and force a slim archive. Skip for musl (different toolchain) and
  // other platforms.
  const isLinuxGlibc =
    process.platform === 'linux' && !(await isMusl()) && !isWin32()
  if (isLinuxGlibc) {
    const ltoFlags = '-flto -ffat-lto-objects'
    cmakeArgs.push(
      `-DCMAKE_C_FLAGS=${ltoFlags}`,
      `-DCMAKE_CXX_FLAGS=${ltoFlags}`,
    )
    logger.info('Enabling fat-object LTO for Linux glibc builds')
  }

  if (process.platform === 'darwin') {
    const targetArch = getEnvValue('TARGET_ARCH') || process.arch
    const osxArch = targetArch === 'x64' ? 'x86_64' : 'arm64'
    cmakeArgs.push(`-DCMAKE_OSX_ARCHITECTURES=${osxArch}`)
    logger.info(
      `Building LIEF for darwin-${targetArch} (CMAKE_OSX_ARCHITECTURES=${osxArch})`,
    )
  }

  appendWindowsLiefCmakeArgs(cmakeArgs)

  // On musl, disable fortify source to avoid glibc-specific fortify functions.
  // musl libc does not provide __*_chk functions (e.g., __snprintf_chk, __memcpy_chk).
  // This prevents linking errors when binject (built on musl) tries to link LIEF.
  // Use -U to undefine first in case it's set elsewhere, then define as 0.
  // The -Wp,-U_FORTIFY_SOURCE passes -U directly to the preprocessor (more reliable).
  const muslLibc = await isMusl()
  if (muslLibc) {
    const fortifyDisableFlags =
      '-Wp,-U_FORTIFY_SOURCE -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0'
    cmakeArgs.push(
      `-DCMAKE_C_FLAGS=${fortifyDisableFlags}`,
      `-DCMAKE_CXX_FLAGS=${fortifyDisableFlags}`,
      `-DCMAKE_C_FLAGS_RELEASE=-O3 -DNDEBUG ${fortifyDisableFlags}`,
      `-DCMAKE_CXX_FLAGS_RELEASE=-O3 -DNDEBUG ${fortifyDisableFlags}`,
    )
    logger.info('Disabling fortify source for musl libc compatibility')
  }

  const ccachePath = await which('ccache')
  if (ccachePath) {
    cmakeArgs.push('-DCMAKE_CXX_COMPILER_LAUNCHER=ccache')
    logger.info('Using ccache for faster compilation')
  } else {
    logger.info('ccache not available, building without cache')
  }

  const muslFortifyFlags = muslLibc
    ? '-Wp,-U_FORTIFY_SOURCE -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0'
    : undefined
  const cleanEnv = {
    LIEF_VERSION_ENV: LIEF_VERSION,
    CFLAGS: appendCCRemapFlags(muslFortifyFlags),
    CXXFLAGS: appendCCRemapFlags(muslFortifyFlags),
    CPPFLAGS: muslLibc ? '-U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0' : undefined,
    LDFLAGS: undefined,
  }
  await runCommand('cmake', cmakeArgs, liefBuildDir, cleanEnv)
  logger.info('')
  return { __proto__: null, cleanEnv, muslLibc }
}

export function getBuiltLiefLibraryPath(liefBuildDir) {
  const archivePath = path.join(liefBuildDir, 'libLIEF.a')
  if (existsSync(archivePath)) {
    return archivePath
  }
  const libraryPath = path.join(liefBuildDir, 'LIEF.lib')
  if (!existsSync(libraryPath)) {
    throw new Error(
      `LIEF library not found (checked libLIEF.a and LIEF.lib in ${liefBuildDir})`,
    )
  }
  return libraryPath
}

async function main() {
  try {
    // Use platform-specific build directory for complete isolation.
    const platformArch = getCurrentLiefPlatformArch()
    const { buildDir, liefBuildDir } = getLiefBuildDirs(platformArch)
    const downloadedDir = getDownloadedLiefDir(platformArch)

    // Check if LIEF exists in any location (local or downloaded).
    const localLibPath = getLiefLibPathAt(liefBuildDir)
    const downloadedLibPath = getLiefLibPathAt(downloadedDir)
    const liefLibPath = localLibPath ?? downloadedLibPath
    const liefAvailable = liefLibPath !== undefined

    // Check if LIEF is already built.
    const forceRebuild = process.argv.includes('--force')
    const checkpointExists = !(await shouldRun(
      buildDir,
      '',
      CHECKPOINTS.LIEF_BUILT,
      forceRebuild,
    ))

    // Validate checkpoint: both checkpoint file AND library file must exist.
    if (checkpointExists && liefAvailable) {
      logger.success('LIEF already built (checkpoint exists)')
      return
    }

    // If checkpoint exists but library is missing, invalidate and rebuild.
    if (checkpointExists && !liefAvailable) {
      logger.info(
        'Checkpoint exists but LIEF library missing, rebuilding from scratch',
      )
    }

    logger.info('🔨 Building LIEF library…')
    logger.error('')

    // Check if LIEF submodule is initialized.
    const liefSourceDir = path.join(packageRoot, 'upstream', 'lief')
    const liefCMakeLists = path.join(liefSourceDir, 'CMakeLists.txt')
    const isLiefBuild = existsSync(liefCMakeLists)

    if (!isLiefBuild) {
      // Not building LIEF itself - download prebuilt.
      logger.info('LIEF submodule not initialized, downloading prebuilt…')
      const checkpointed = await checkpointPrebuiltLief({
        platformArch,
        buildDir,
      })
      if (checkpointed) {
        return
      }

      // Prebuilt download failed - cannot continue.
      throw new Error(
        'Failed to download prebuilt LIEF. Run: node scripts/fleet/git-partial-submodule.mts clone packages/lief-builder/upstream/lief',
      )
    }

    logger.info(
      `Building LIEF on ${process.platform} for cross-platform binary injection support`,
    )

    // Create build directory.
    await safeMkdir(buildDir)

    // Copy LIEF source from upstream to build directory.
    // This allows patching without modifying the git submodule.
    await copyLiefSource(liefPatchedDir)
    logger.info('LIEF source ready')

    // Apply Socket patches to LIEF source (e.g., remove 1MB note size limit).
    await applyLiefPatches(liefPatchedDir)

    // Create build directory.
    await safeMkdir(liefBuildDir)

    const { cleanEnv, muslLibc } = await configureLiefBuild(liefBuildDir)

    // Build LIEF with parallel compilation.
    // Use 90% of available CPUs for faster builds (CI environments can use full resources).
    const cpuCount = os.cpus().length
    const jobCount = Math.max(1, Math.floor(cpuCount * 0.9))
    logger.info(
      `Building LIEF with ${jobCount} parallel jobs (${cpuCount} CPUs available)...`,
    )
    const buildStart = Date.now()
    // Use the same cleanEnv to ensure subdependencies get the flags.
    await runCommand(
      'cmake',
      ['--build', '.', '--config', 'Release', `-j${jobCount}`],
      liefBuildDir,
      cleanEnv,
    )
    const buildDuration = Math.round((Date.now() - buildStart) / 1000)
    logger.info(
      `LIEF build completed in ${buildDuration}s (${Math.floor(buildDuration / 60)}m ${buildDuration % 60}s)`,
    )
    logger.info('')

    logger.success('LIEF build completed successfully!')

    // Verify library exists (platform-specific naming).
    // When using clang on Windows with Ninja/Unix Makefiles, it produces LIEF.lib (MSVC-style)
    // When using gcc/MinGW on Windows, it produces libLIEF.a (Unix-style)
    // On Unix platforms: libLIEF.a
    const libPath = getBuiltLiefLibraryPath(liefBuildDir)

    // Multiple fs.stat() calls consume stats.size for downloaded-archive /
    // built-library size reporting and minimum-size quick checks.
    // oxlint-disable-next-line socket/prefer-exists-sync -- see above
    const stats = await fs.stat(libPath)
    const sizeMB = (stats.size / 1024 / 1024).toFixed(2)
    logger.info(`LIEF library size: ${sizeMB} MB`)

    // Verify musl compatibility immediately after build (fail fast).
    const compatibility = await verifyMuslCompatibility(libPath)
    if (!compatibility.compatible) {
      throw new Error(
        `LIEF build produced musl-incompatible library: ${compatibility.reason}. ` +
          'This indicates _FORTIFY_SOURCE was not properly disabled during compilation.',
      )
    }
    if (muslLibc) {
      logger.success(
        'LIEF library verified musl-compatible (no glibc fortify symbols)',
      )
    }

    // Copy upstream headers to build directory for standalone distribution.
    // The checkpoint tarball needs to include all headers required for compilation,
    // not just the generated config.h and version.h from the CMake build.
    logger.info('Copying upstream headers for standalone distribution…')
    const upstreamIncludeDir = path.join(liefUpstream, 'include', 'LIEF')
    const buildIncludeDir = path.join(liefBuildDir, 'include', 'LIEF')

    // Ensure target directory exists.
    await safeMkdir(buildIncludeDir)

    // Recursively copy all header files from upstream to build directory.
    // Parallelized for better performance with many files.
    const copyUpstreamHeaders = async (src, dest) => {
      const entries = await fs.readdir(src, { withFileTypes: true })

      // Separate directories and files for parallel processing.
      const dirs = entries.filter(e => e.isDirectory())
      const files = entries.filter(
        e =>
          e.name.endsWith('.hpp') ||
          e.name.endsWith('.h') ||
          e.name.endsWith('.def'),
      )

      // Create directories first (needed before file copies).
      const dirResults = await Promise.allSettled(
        dirs.map(dir => safeMkdir(path.join(dest, dir.name))),
      )
      const dirFailed = dirResults.filter(r => r.status === 'rejected')
      if (dirFailed.length > 0) {
        throw new Error(
          `Failed to create ${dirFailed.length} directories: ${dirFailed.map(r => r.reason?.message || r.reason).join(', ')}`,
        )
      }

      // Copy files and recurse into subdirectories in parallel.
      const copyResults = await Promise.allSettled([
        ...files.map(file =>
          fs.copyFile(path.join(src, file.name), path.join(dest, file.name)),
        ),
        ...dirs.map(dir =>
          copyUpstreamHeaders(
            path.join(src, dir.name),
            path.join(dest, dir.name),
          ),
        ),
      ])
      const copyFailed = copyResults.filter(r => r.status === 'rejected')
      if (copyFailed.length > 0) {
        throw new Error(
          `Failed to copy ${copyFailed.length} files/directories: ${copyFailed.map(r => r.reason?.message || r.reason).join(', ')}`,
        )
      }
    }

    await copyUpstreamHeaders(upstreamIncludeDir, buildIncludeDir)
    logger.success('Upstream headers copied to build directory')
    logger.info('')

    // Create checkpoint.
    const lbTargetArch = getEnvValue('TARGET_ARCH') || process.arch
    const lbTargetPlatform = isWin32() ? 'win' : process.platform
    await createCheckpoint(
      buildDir,
      CHECKPOINTS.LIEF_BUILT,
      async () => {
        // Verify library exists and has reasonable size.
        // oxlint-disable-next-line socket/prefer-exists-sync -- multiple fs.stat() calls consume stats.size for downloaded-archive / built-library size reporting and minimum-size quick checks.
        const libStats = await fs.stat(libPath)
        if (libStats.size < 1_000_000) {
          throw new Error(
            `LIEF library too small: ${libStats.size} bytes (expected >1MB)`,
          )
        }

        // Verify all required files exist.
        const verification = verifyLiefAt(liefBuildDir)
        if (!verification.valid) {
          throw new Error(
            `Incomplete LIEF build - missing files:\n  ${verification.missing.join('\n  ')}`,
          )
        }
      },
      {
        arch: lbTargetArch,
        artifactPath: liefBuildDir,
        buildDir: path.relative(packageRoot, liefBuildDir),
        libPath: path.relative(buildDir, libPath),
        libSize: stats.size,
        libSizeMB: sizeMB,
        platform: lbTargetPlatform,
        platformArch,
        version: LIEF_VERSION,
      },
    )
  } catch (e) {
    logger.info('')
    logger.fail(`LIEF build failed: ${errorMessage(e)}`)
    try {
      const { logTransientErrorHelp } =
        await import('local-build-infra/lib/github-error-utils')
      await logTransientErrorHelp(e)
    } catch {
      // Hint module failed to load — original error already logged.
    }
    throw e
  }
}

// Run main only when executed directly (not when imported).
// IMPORTANT: Only check import.meta.url match - DO NOT use generic endsWith('build.mts')
// because that would incorrectly trigger main() when other packages (binject, binpress)
// import this module from their own build.mts scripts.
const isMainModule =
  fileURLToPath(import.meta.url) === path.resolve(process.argv[1])

if (isMainModule) {
  main().catch(e => {
    logger.error(errorMessage(e))
    process.exitCode = 1
  })
}
