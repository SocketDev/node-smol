/**
 * Submodule version and checksum readers for .gitmodules.
 *
 * Reads the fleet pin format through the fleet's OWN parser
 * (scripts/fleet/_shared/gitmodules.mts — the same module family that WRITES
 * the pins via gen/gitmodules-hash), so this reader can never drift from the
 * writer: `branch =` carries the release-tag pin the version derives from,
 * and the `# <slug>-<version> (<date>) sha256:<hex>` header comment carries
 * the archive content-hash.
 */

import { readFileSync } from 'node:fs'
import path from 'node:path'

import { PACKAGE_ROOT } from './constants.mts'
import { isErrnoException } from '@socketsecurity/lib-stable/errors/predicates'
import { coerceVersion } from '@socketsecurity/lib-stable/versions/parse'

import { parseGitmodules } from '../../../scripts/fleet/_shared/gitmodules.mts'

import type { GitmodulesEntry } from '../../../scripts/fleet/_shared/gitmodules.mts'

/**
 * Extract submodule checksum from the .gitmodules header comment.
 *
 * The fleet pin comment is `# <slug>-<version> (<date>) sha256:<hex>`; the
 * fleet parser surfaces the hash as `headerSha`. Returns undefined when the
 * block carries no checksum (it is optional).
 *
 * @example
 *   const checksum = getSubmoduleChecksum(
 *     'packages/node-smol-builder/upstream/node',
 *     'node',
 *   )
 *   // Returns: { algorithm: 'sha256', hash: '55978d1460...' }
 */
export function getSubmoduleChecksum(
  submodulePath: string,
  packageName: string,
): { algorithm: string; hash: string } | undefined {
  if (!packageName || packageName.trim() === '') {
    throw new Error('Package name cannot be empty')
  }
  const entry = readSubmoduleEntry(submodulePath)
  if (!entry.headerSha) {
    return undefined
  }
  // The fleet header hash is sha256 by definition (gen/gitmodules-hash writes
  // only that form).
  return { algorithm: 'sha256', hash: entry.headerSha }
}

/**
 * Extract a submodule's pinned release version from .gitmodules.
 *
 * The `branch =` field IS the release-tag pin (the fleet pins upstreams at
 * tags), normalized to a bare semver through the lib's coerceVersion so a
 * v-prefixed tag (`v26.7.0`, node's convention) and a bare one (`1.7.19`,
 * lief's) both read the same.
 *
 * @throws {Error} When the block is missing, carries no branch pin, or the
 *   branch does not coerce to a version.
 */
export function getSubmoduleVersion(
  submodulePath: string,
  packageName: string,
): string {
  if (!packageName || packageName.trim() === '') {
    throw new Error('Package name cannot be empty')
  }
  const entry = readSubmoduleEntry(submodulePath)
  if (!entry.branch) {
    throw new Error(
      `No branch pin for submodule '${submodulePath}' in .gitmodules\n` +
        'The fleet pins upstreams at release tags via `branch =`; a ref-only ' +
        'block has no version to read.',
    )
  }
  const version = coerceVersion(entry.branch)
  if (!version) {
    throw new Error(
      `Unparseable version for submodule '${submodulePath}'\n` +
        `  Saw: branch = ${entry.branch}; wanted a release tag that coerces ` +
        'to semver (v26.7.0, 1.7.19, …).',
    )
  }
  return version
}

/**
 * Read and parse the monorepo root .gitmodules, returning the entry whose
 * `path =` matches. Throws a What/Where/Saw/Fix error when the file or the
 * entry is missing — a build must never guess a pin.
 */
export function readSubmoduleEntry(submodulePath: string): GitmodulesEntry {
  const gitmodulesPath = path.join(PACKAGE_ROOT, '..', '..', '.gitmodules')

  let content
  try {
    content = readFileSync(gitmodulesPath, 'utf8')
  } catch (e) {
    if (isErrnoException(e) && e.code === 'ENOENT') {
      throw new Error(
        `.gitmodules not found at: ${gitmodulesPath}\n` +
          'This function must be called from within a monorepo package.',
        { cause: e },
      )
    }
    throw e
  }

  const entry = parseGitmodules(content).find(
    candidate => candidate.path === submodulePath,
  )
  if (!entry) {
    throw new Error(
      `Submodule '${submodulePath}' not found in .gitmodules\n` +
        `Expected a block whose path is: ${submodulePath}`,
    )
  }
  return entry
}
