/**
 * Generic upstream-tree init helper.
 *
 * Materializes a single `.gitmodules`-declared upstream tree on demand, with
 * a sentinel-file check so repeat calls are no-ops. Used by
 * setup-build-toolchain scripts that need a vendored upstream tree to exist
 * before the rest of the build can run.
 *
 * This repo tracks NO gitlinks: upstream trees are untracked-by-default
 * (docs/agents.md/fleet/untracked-by-default.md) and restored from the
 * `.gitmodules` ref/branch/sparse metadata by
 * `scripts/fleet/git-partial-submodule.mts clone`. A raw
 * `git submodule update --init` has no index entry to read and fails with
 * "pathspec did not match any file(s) known to git", so this helper must
 * always go through the fleet tool.
 *
 * `ensureLibdeflate` / `ensureZstd` / binject's `ensureCjson` are thin
 * wrappers over `ensureSubmodule` — add new builders the same way instead of
 * forking the shell logic.
 */

import { existsSync } from 'node:fs'
import path from 'node:path'
import process from 'node:process'

import { getDefaultLogger } from '@socketsecurity/lib-stable/logger/default'
import { spawn } from '@socketsecurity/lib-stable/process/spawn/child'

import { errorMessage } from './error-utils.mts'

const logger = getDefaultLogger()

/**
 * Ensure an upstream submodule tree is materialized.
 *
 * @param {object} options
 * @param {string} options.name - Submodule name for log/error messages
 *   (e.g., "yoga", "opentui").
 * @param {string} options.submodulePath - Path FROM the monorepo root to the
 *   submodule directory (e.g., "packages/yoga-layout-builder/upstream/yoga").
 * @param {string} options.sentinelFile - Path INSIDE the submodule to a
 *   file whose existence proves it's checked out (e.g., "yoga/Yoga.h").
 * @param {string} options.monorepoRoot - Absolute path to the
 *   monorepo root containing .git and .gitmodules.
 *
 * @returns {Promise<void>}
 */
export async function ensureSubmodule({
  monorepoRoot,
  name,
  sentinelFile,
  submodulePath,
}) {
  const absSubmodule = path.join(monorepoRoot, submodulePath)
  const absSentinel = path.join(absSubmodule, sentinelFile)

  if (existsSync(absSentinel)) {
    return
  }

  const cloneCommand = `node scripts/fleet/git-partial-submodule.mts clone ${submodulePath}`
  const gitDir = path.join(monorepoRoot, '.git')
  if (!existsSync(gitDir)) {
    throw new Error(
      `${name} source not found at ${absSubmodule} and no .git directory available.\n` +
        `In Docker builds, ensure the CI workflow materializes the ${name} tree before docker build:\n` +
        `  ${cloneCommand}`,
    )
  }

  logger.info(`Materializing ${name} upstream tree…`)
  logger.log(`Running: ${cloneCommand}`)

  let result
  try {
    result = await spawn(
      process.execPath,
      [
        path.join(monorepoRoot, 'scripts/fleet/git-partial-submodule.mts'),
        'clone',
        submodulePath,
      ],
      { cwd: monorepoRoot, stdio: 'inherit' },
    )
  } catch (e) {
    throw new Error(
      `${name} upstream tree not materialized and the clone command failed.\n` +
        `Run: ${cloneCommand}\n` +
        `Error: ${errorMessage(e)}`,
      { cause: e },
    )
  }

  const exit = result.code ?? result.exitCode ?? 0
  if (exit !== 0) {
    throw new Error(
      `Failed to materialize ${name} upstream tree (exit code ${exit}).\n` +
        `Run: ${cloneCommand}`,
    )
  }

  if (!existsSync(absSentinel)) {
    throw new Error(
      `${name} clone completed but sentinel ${sentinelFile} is missing. ` +
        `The .gitmodules entry may not be properly configured.`,
    )
  }

  logger.success(`${name} upstream tree materialized`)
}
