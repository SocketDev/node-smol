/**
 * Patch Validation Utilities.
 *
 * Provides utilities for validating and applying patches to source code.
 */

import { promises as fs } from 'node:fs'
import path from 'node:path'
import process from 'node:process'

import { which } from '@socketsecurity/lib-stable/exe/path/which'
import { getDefaultLogger } from '@socketsecurity/lib-stable/logger/default'
import { spawn } from '@socketsecurity/lib-stable/process/spawn/child'
import { isSpawnError } from '@socketsecurity/lib-stable/process/spawn/errors'

import { printError } from './build-output.mts'

export {
  analyzePatchContent,
  checkPatchConflicts,
  parsePatchFileModifications,
  rangesOverlap,
} from './patch-analysis.mts'
export type {
  PatchConflict,
  PatchFileData,
  PatchLineRange,
} from './patch-analysis.mts'

const logger = getDefaultLogger()

const PATCH_NOT_FOUND_MSG = 'patch not found in PATH'
const PATCH_WINDOWS_NOT_FOUND_MSG =
  'No supported GNU patch (version 2.7 or newer) found in PATH. ' +
  'Install GNU patch 2.7 or newer and ensure it is available in PATH.'
const PATCH_DIAGNOSTIC_LIMIT = 2000
const PATCH_VERSION_PROBE_TIMEOUT = 5000
const PATCH_VERSION_PROBE_MAX_BUFFER = 64 * 1024

/**
 * Apply a patch file.
 *
 * @param {string} patchFile - Path to patch file.
 * @param {string} targetDir - Directory to apply patch to.
 *
 * @returns {Promise<void>}
 */
export async function applyPatch(
  patchFile: string,
  targetDir: string,
): Promise<void> {
  logger.info(`Applying ${path.basename(patchFile)}`)

  const patchPath = await resolvePatchExecutable()

  // Resolve to absolute path since we're changing cwd
  const absolutePatchFile = path.resolve(patchFile)

  let result: PatchSpawnOutcome
  try {
    result = await spawn(
      patchPath,
      ['-p1', '--batch', '-i', absolutePatchFile],
      {
        cwd: targetDir,
        env: process.env,
      },
    )
  } catch (spawnError) {
    // spawn() throws when command exits with non-zero code
    if (!isSpawnError(spawnError)) {
      throw spawnError
    }
    result = spawnError
  }

  const exitCode = result.code ?? 0
  if (exitCode !== 0) {
    throw new Error(
      `Failed to apply patch: ${patchFile} (exit ${exitCode})` +
        formatPatchDiagnostic('stderr', result.stderr) +
        formatPatchDiagnostic('stdout', result.stdout),
    )
  }
}

/**
 * Apply all patches in a directory.
 *
 * @param {string} patchDir - Directory containing patch files.
 * @param {string} targetDir - Directory to apply patches to.
 * @param {object} options - Options.
 * @param {boolean} options.validate - Validate patches before applying
 *   (default: true)
 *
 * @returns {Promise<void>}
 */
export async function applyPatchDirectory(
  patchDir: string,
  targetDir: string,
  { validate = true }: { validate?: boolean | undefined } = {},
): Promise<void> {
  logger.substep('Applying patches')

  const entries = await fs.readdir(patchDir, { withFileTypes: true })
  const patchFiles = entries
    .filter(entry => entry.isFile() && entry.name.endsWith('.patch'))
    .map(entry => path.join(patchDir, entry.name))
    .toSorted()

  if (!patchFiles.length) {
    logger.info('No patches found')
    return
  }

  // Validate all patches in parallel (validation is read-only).
  if (validate) {
    const validationResults = await Promise.allSettled(
      patchFiles.map(async patchFile => {
        const isValid = await validatePatch(patchFile, targetDir)
        if (!isValid) {
          throw new Error(`Patch validation failed: ${patchFile}`)
        }
        return patchFile
      }),
    )
    const failures = validationResults.filter(
      (r): r is PromiseRejectedResult => r.status === 'rejected',
    )
    if (failures.length > 0) {
      const failedPatches = failures
        .map(r => r.reason?.message || r.reason)
        .join('\n')
      throw new Error(
        `${failures.length} patch(es) failed validation:\n${failedPatches}`,
      )
    }
  }

  // Apply patches in order.
  for (let i = 0, { length } = patchFiles; i < length; i += 1) {
    const patchFile = patchFiles[i]
    if (patchFile === undefined) {
      continue
    }
    // eslint-disable-next-line no-await-in-loop
    await applyPatch(patchFile, targetDir)
  }

  logger.info(`Applied ${patchFiles.length} patches`)
}

/**
 * Create a patch file from git diff.
 *
 * @param {string} repoDir - Git repository directory.
 * @param {string} outputFile - Output patch file path.
 * @param {object} options - Options.
 * @param {boolean} options.staged - Only include staged changes (default:
 *   false)
 *
 * @returns {Promise<void>}
 */
export async function createPatchFromGit(
  repoDir: string,
  outputFile: string,
  { staged = false }: { staged?: boolean | undefined } = {},
): Promise<void> {
  logger.substep('Creating patch from git diff')

  const gitPath = await which('git', { nothrow: true })
  if (typeof gitPath !== 'string') {
    throw new Error('git not found in PATH')
  }

  const args = ['diff']
  if (staged) {
    args.push('--cached')
  }

  const result = await spawn(gitPath, args, {
    cwd: repoDir,
  })

  const stdout = result.stdout ?? ''
  if (!stdout.trim()) {
    throw new Error('No changes to create patch from')
  }

  await fs.writeFile(outputFile, stdout, 'utf8')

  logger.info(`Created patch: ${path.basename(outputFile)}`)
}

/**
 * The subset of a spawn result / spawn error the patch runners inspect.
 */
export interface PatchSpawnOutcome {
  code?: number | undefined
  stderr?: string | Buffer | undefined
  stdout?: string | Buffer | undefined
}

export interface PatchExecutableResolverDependencies {
  platform?: NodeJS.Platform | undefined
  locate?: typeof which | undefined
  probe?: typeof spawn | undefined
}

export async function findWindowsPatchCandidates(
  locate: typeof which,
): Promise<string[]> {
  try {
    const candidates = await locate('patch', { all: true, nothrow: true })
    return Array.isArray(candidates) ? candidates : []
  } catch {
    return []
  }
}

export function formatPatchDiagnostic(
  label: 'stderr' | 'stdout',
  output: string | Buffer | undefined,
): string {
  if (!output) {
    return ''
  }
  const text = String(output)
  const excerpt =
    text.length > PATCH_DIAGNOSTIC_LIMIT
      ? `${text.slice(-PATCH_DIAGNOSTIC_LIMIT)}\n[${text.length - PATCH_DIAGNOSTIC_LIMIT} characters omitted]`
      : text
  return `\n${label}: ${excerpt}`
}

export function isSupportedGnuPatchVersion(versionOutput: string): boolean {
  // The version line has a GNU patch label, major, minor, and optional patch.
  const versionMatch = /\bGNU patch\s+(\d+)\.(\d+)(?:\.(\d+))?/i.exec(
    versionOutput,
  )
  if (!versionMatch) {
    return false
  }
  const majorVersion = Number(versionMatch[1])
  const minorVersion = Number(versionMatch[2])
  return majorVersion > 2 || (majorVersion === 2 && minorVersion >= 7)
}

export async function isSupportedPatchCandidate(
  candidate: string,
  probe: typeof spawn,
): Promise<boolean> {
  if (!path.win32.isAbsolute(candidate)) {
    return false
  }
  try {
    const result = await probe(candidate, ['--version'], {
      encoding: 'utf8',
      maxBuffer: PATCH_VERSION_PROBE_MAX_BUFFER,
      timeout: PATCH_VERSION_PROBE_TIMEOUT,
    })
    if (result.code !== 0) {
      return false
    }
    const versionOutput = `${String(result.stdout ?? '')}\n${String(result.stderr ?? '')}`
    return isSupportedGnuPatchVersion(versionOutput)
  } catch {
    return false
  }
}

/**
 * Resolve patch from PATH, selecting a supported GNU patch on Windows.
 */
export async function resolvePatchExecutable(
  dependencies: PatchExecutableResolverDependencies = {},
): Promise<string> {
  const platform = dependencies.platform ?? process.platform
  const locate = dependencies.locate ?? which
  if (platform !== 'win32') {
    const patchPath = await locate('patch', { nothrow: true })
    if (typeof patchPath !== 'string') {
      throw new Error(PATCH_NOT_FOUND_MSG)
    }
    return patchPath
  }

  const probe = dependencies.probe ?? spawn
  const candidates = await findWindowsPatchCandidates(locate)
  for (const candidate of candidates) {
    if (await isSupportedPatchCandidate(candidate, probe)) {
      return candidate
    }
  }

  throw new Error(PATCH_WINDOWS_NOT_FOUND_MSG)
}

/**
 * Revert a patch that has been applied.
 *
 * @param {string} patchFile - Path to patch file.
 * @param {string} targetDir - Directory to revert patch from.
 *
 * @returns {Promise<void>}
 */
export async function revertPatch(
  patchFile: string,
  targetDir: string,
): Promise<void> {
  logger.info(`Reverting ${path.basename(patchFile)}`)

  const patchPath = await resolvePatchExecutable()

  // Resolve to absolute path since we're changing cwd
  const absolutePatchFile = path.resolve(patchFile)

  let result: PatchSpawnOutcome
  try {
    result = await spawn(
      patchPath,
      ['-p1', '--batch', '--reverse', '-i', absolutePatchFile],
      {
        cwd: targetDir,
        env: process.env,
      },
    )
  } catch (spawnError) {
    // spawn() throws when command exits with non-zero code
    if (!isSpawnError(spawnError)) {
      throw spawnError
    }
    result = spawnError
  }

  const exitCode = result.code ?? 0
  if (exitCode !== 0) {
    throw new Error(`Failed to revert patch: ${patchFile}`)
  }
}

/**
 * Test if a patch has already been applied.
 *
 * @param {string} patchFile - Path to patch file.
 * @param {string} targetDir - Directory to check.
 *
 * @returns {Promise<boolean>}
 */
export async function testPatchApplication(
  patchFile: string,
  targetDir: string,
): Promise<boolean> {
  try {
    const patchPath = await resolvePatchExecutable()

    // Resolve to absolute path
    const absolutePatchFile = path.resolve(patchFile)

    let result: PatchSpawnOutcome
    try {
      result = await spawn(
        patchPath,
        ['-p1', '--batch', '--dry-run', '--reverse', '-i', absolutePatchFile],
        {
          cwd: targetDir,
          env: process.env,
        },
      )
    } catch (spawnError) {
      // spawn() throws when command exits with non-zero code
      if (!isSpawnError(spawnError)) {
        throw spawnError
      }
      result = spawnError
    }

    // If reverse patch succeeds, the patch has been applied.
    return (result.code ?? 0) === 0
  } catch {
    return false
  }
}

/**
 * Validate a patch file can be applied cleanly.
 *
 * @param {string} patchFile - Path to patch file.
 * @param {string} targetDir - Directory to apply patch to.
 *
 * @returns {Promise<boolean>}
 */
export async function validatePatch(
  patchFile: string,
  targetDir: string,
): Promise<boolean> {
  try {
    const patchPath = await resolvePatchExecutable()

    // Resolve to absolute path
    const absolutePatchFile = path.resolve(patchFile)

    let result: PatchSpawnOutcome
    try {
      result = await spawn(
        patchPath,
        ['-p1', '--batch', '--dry-run', '-i', absolutePatchFile],
        {
          cwd: targetDir,
          env: process.env,
        },
      )
    } catch (spawnError) {
      // spawn() throws when command exits with non-zero code
      if (!isSpawnError(spawnError)) {
        throw spawnError
      }
      result = spawnError
    }

    const exitCode = result.code ?? 0
    if (exitCode !== 0) {
      printError(`Patch validation failed: ${patchFile}`)
      if (result.stderr) {
        printError(`stderr: ${String(result.stderr)}`)
      }
      if (result.stdout) {
        printError(`stdout: ${String(result.stdout)}`)
      }
      return false
    }

    return true
  } catch (e) {
    printError(`Patch validation error: ${patchFile}`, e)
    return false
  }
}
