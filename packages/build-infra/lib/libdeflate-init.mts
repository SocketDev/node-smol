/**
 * Shared utility for initializing the libdeflate upstream tree on-demand.
 * Used by binject for high-performance gzip compression on Linux/Windows.
 *
 * Thin wrapper over `ensureSubmodule`, which restores the tree through
 * `scripts/fleet/git-partial-submodule.mts clone` — this repo tracks no
 * gitlinks, so raw `git submodule update --init` cannot restore anything.
 */

import path from 'node:path'

import { ensureSubmodule } from './submodule-init.mts'

/**
 * Ensure the libdeflate upstream tree is materialized.
 *
 * @param {object} options - Initialization options.
 * @param {string} options.packageDir - Package directory path (binject
 *   package).
 *
 * @returns {Promise<void>}
 */
export async function ensureLibdeflate({ packageDir }) {
  await ensureSubmodule({
    monorepoRoot: path.resolve(packageDir, '../..'),
    name: 'libdeflate',
    sentinelFile: 'libdeflate.h',
    submodulePath: 'packages/binject/upstream/libdeflate',
  })
}
