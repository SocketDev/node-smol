/**
 * Shared tarball extraction utilities.
 *
 * Provides secure tarball extraction with path traversal protection
 * and cross-platform Windows/Unix compatibility.
 */

import path from 'node:path'

import { safeMkdir } from '@socketsecurity/lib-stable/fs/safe'
import { normalizePath } from '@socketsecurity/lib-stable/paths/normalize'
import { spawn } from '@socketsecurity/lib-stable/process/spawn/child'

import { getTarCapabilities } from './platform-mappings.mts'
import type { TarCapabilities } from './platform-mappings.mts'
import { whichRequired } from './which-required.mts'

/**
 * Securely extract a tarball to a directory. Archive paths are validated
 * against traversal before extraction, and --no-absolute-names is added on
 * platforms whose tar supports it as defense in depth.
 *
 * @param {string} tarballPath - Path to the tarball file.
 * @param {string} extractDir - Directory to extract to.
 * @param {object} options - See ExtractTarballOptions.
 *
 * @returns {Promise<string[]>} File paths from the validation pass; empty
 *   when validation is disabled.
 *
 * @throws {Error} If extraction fails or paths are unsafe.
 *   Full discussion: docs/agents.md/repo/build-toolchain.md.
 */
export interface ExtractTarballOptions {
  createDir?: boolean | undefined
  overwrite?: boolean | undefined
  stdio?: 'ignore' | 'inherit' | 'pipe' | undefined
  stripComponents?: number | undefined
  validate?: boolean | undefined
}

export async function appendSupportedTarOptions(
  tarArgs: string[],
  options: {
    capabilities?: TarCapabilities | undefined
    overwrite?: boolean | undefined
    tarBin?: string | undefined
  } = {},
): Promise<void> {
  const {
    capabilities,
    overwrite = false,
    tarBin = 'tar',
  } = {
    __proto__: null,
    ...options,
  }
  const tarCapabilities = capabilities ?? (await getTarCapabilities(tarBin))
  if (tarCapabilities.noAbsoluteNames) {
    tarArgs.push('--no-absolute-names')
  }
  if (overwrite && tarCapabilities.overwrite) {
    tarArgs.push('--overwrite')
  }
}

export async function extractTarball(
  tarballPath: string,
  extractDir: string,
  options: ExtractTarballOptions = {},
): Promise<string[]> {
  const {
    createDir = true,
    overwrite = true,
    stdio = 'ignore',
    stripComponents = 0,
    validate = true,
  } = options

  // Create extraction directory if needed.
  if (createDir) {
    await safeMkdir(extractDir)
  }

  // Validate tarball paths before extraction.
  let files: string[] = []
  const tarBin = await whichRequired('tar')
  const tarCapabilities = await getTarCapabilities(tarBin)
  if (validate) {
    files = await validateTarballPathsWithTar(
      tarballPath,
      tarBin,
      tarCapabilities,
    )
  }

  const normalizedTarballPath = normalizePath(tarballPath)
  const normalizedExtractDir = normalizePath(extractDir)

  // Build extraction arguments.
  const tarArgs = [
    ...(tarCapabilities.forceLocal ? ['--force-local'] : []),
    '-xzf',
    normalizedTarballPath,
    '-C',
    normalizedExtractDir,
  ]

  // Strip leading path components if requested.
  if (stripComponents > 0) {
    tarArgs.push(`--strip-components=${stripComponents}`)
  }

  await appendSupportedTarOptions(tarArgs, {
    capabilities: tarCapabilities,
    overwrite,
  })

  // Extract.
  const result = await spawn(tarBin, tarArgs, { stdio })
  if (result.code !== 0) {
    throw new Error(`tar extraction failed with exit code ${result.code}`)
  }

  return files
}

/**
 * Validate tarball contents for path traversal attacks.
 * Throws if any file in the tarball has an unsafe path.
 *
 * @param {string} tarballPath - Path to the tarball file.
 *
 * @returns {Promise<string[]>} Array of file paths in the tarball.
 *
 * @throws {Error} If tarball contains unsafe paths.
 */
export async function validateTarballPaths(
  tarballPath: string,
): Promise<string[]> {
  const tarBin = await whichRequired('tar')
  const tarCapabilities = await getTarCapabilities(tarBin)
  return validateTarballPathsWithTar(tarballPath, tarBin, tarCapabilities)
}

export async function validateTarballPathsWithTar(
  tarballPath: string,
  tarBin: string,
  capabilities: TarCapabilities,
): Promise<string[]> {
  const normalizedTarballPath = normalizePath(tarballPath)

  // List tarball contents.
  const listArgs = [
    ...(capabilities.forceLocal ? ['--force-local'] : []),
    '-tzf',
    normalizedTarballPath,
  ]
  const listResult = await spawn(tarBin, listArgs, { stdio: 'pipe' })
  if (listResult.code !== 0) {
    throw new Error(`tar listing failed with exit code ${listResult.code}`)
  }

  const files = (listResult.stdout || '')
    .split(/\r?\n/)
    .filter(Boolean)
    .map(f => f.trim())

  // Check for path traversal attempts.
  for (let i = 0, { length } = files; i < length; i += 1) {
    const file = files[i]
    if (file === undefined) {
      continue
    }
    // Check for null bytes (can bypass filesystem checks)
    if (file.includes('\0')) {
      throw new Error(
        `Tarball contains unsafe path: ${file} (null byte detected)`,
      )
    }

    // Reject leading / or \ before platform-specific absolute-path check.
    // On Windows, path.isAbsolute('/etc/passwd') returns false (a drive
    // letter or UNC prefix is required) — so a tarball entry starting with
    // `/` would slip past the host-flavor isAbsolute check even though some
    // tar implementations (Git Bash, WSL) happily strip the leading slash
    // and write to the root of extractDir. Platform-independent rejection
    // closes that gap.
    if (
      normalizePath(file).startsWith('/') ||
      normalizePath(file).startsWith('\\')
    ) {
      throw new Error(
        `Tarball contains unsafe path: ${file} (absolute path detected)`,
      )
    }

    // Check for absolute paths BEFORE normalization
    // (catches Windows drive letters like C:\ and Unix absolute paths like /)
    if (path.isAbsolute(file)) {
      throw new Error(
        `Tarball contains unsafe path: ${file} (absolute path detected)`,
      )
    }

    // Normalize and check for parent directory traversal
    const normalized = path.normalize(file)
    if (normalized.startsWith('..')) {
      throw new Error(
        `Tarball contains unsafe path: ${file} (path traversal attempt detected)`,
      )
    }

    // Defense-in-depth: Check normalized path doesn't become absolute
    // This catches edge cases where normalization might produce absolute paths.
    if (path.isAbsolute(normalized)) {
      throw new Error(
        `Tarball contains unsafe path: ${file} (normalizes to absolute path)`,
      )
    }
  }

  return files
}
