/**
 * Shared utility for initializing the zstd upstream tree on-demand.
 * Used by binject, binpress, binflate, bin-stub-builder, and other packages
 * that need zstd compression/decompression.
 *
 * Thin wrapper over `ensureSubmodule`, which restores the tree through
 * `scripts/fleet/git-partial-submodule.mts clone` — this repo tracks no
 * gitlinks, so raw `git submodule update --init` cannot restore anything.
 */

import path from 'node:path'

import { ensureSubmodule } from './submodule-init.mts'

/**
 * Ensure the zstd upstream tree is materialized.
 *
 * @param {object} options - Initialization options.
 * @param {string} options.packageDir - Directory of the CALLING package
 *   (e.g., binject); the zstd tree itself lives under bin-infra.
 *
 * @returns {Promise<void>}
 */
export async function ensureZstd({ packageDir }) {
  await ensureSubmodule({
    monorepoRoot: path.resolve(packageDir, '../..'),
    name: 'zstd',
    sentinelFile: path.join('lib', 'zstd.h'),
    submodulePath: 'packages/bin-infra/upstream/zstd',
  })
}
