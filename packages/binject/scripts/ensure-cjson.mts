/**
 * Utility for initializing the cJSON upstream tree on-demand.
 * Used by binject for JSON parsing of sea-config.json (smol config extraction).
 *
 * Thin wrapper over build-infra's `ensureSubmodule`, which restores the tree
 * through `scripts/fleet/git-partial-submodule.mts clone` — this repo tracks
 * no gitlinks, so raw `git submodule update --init` cannot restore anything.
 */

import path from 'node:path'

import { ensureSubmodule } from 'build-infra/lib/submodule-init'

/**
 * Ensure the cJSON upstream tree is materialized.
 *
 * @param {object} options - Initialization options.
 * @param {string} options.packageDir - Package directory path (binject
 *   package).
 *
 * @returns {Promise<void>}
 */
export async function ensureCjson({ packageDir }: { packageDir: string }) {
  await ensureSubmodule({
    monorepoRoot: path.resolve(packageDir, '../..'),
    name: 'cJSON',
    sentinelFile: 'cJSON.h',
    submodulePath: 'packages/binject/upstream/cJSON',
  })
}
