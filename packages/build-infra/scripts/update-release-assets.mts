#!/usr/bin/env node
/**
 * Update checksums from GitHub releases to release-assets.json.
 *
 * This script fetches checksums.txt from the latest release of each tool
 * and updates the embedded checksums in release-assets.json.
 *
 * Usage:
 * node scripts/update-release-assets.mts [--tool=<tool>] [--force]
 *
 * Options: --tool=<name> Only sync specific tool (lief, curl, stubs, binpress,
 * binflate, binject, boringssl, node-smol, ink, iocraft, onnxruntime, models,
 * yoga-layout, opentui) --force Force update even if checksums haven't changed
 * --dry-run Show what would be updated without writing.
 */

import { existsSync, promises as fs, readFileSync } from 'node:fs'
import path from 'node:path'
import process from 'node:process'

import { fileURLToPath } from 'node:url'

import { safeDelete, safeMkdir } from '@socketsecurity/lib-stable/fs/safe'
import {
  getDefaultFormatting,
  stringifyWithFormatting,
} from '@socketsecurity/lib-stable/json/format'
import { getDefaultLogger } from '@socketsecurity/lib-stable/logger/default'
import { isPlainObject } from '@socketsecurity/lib-stable/objects/predicates'
import { getLatestRelease } from '@socketsecurity/lib-stable/releases/github-listing'
import { downloadReleaseAsset } from '@socketsecurity/lib-stable/releases/github-downloads'
import { SOCKET_BTM_REPO } from '@socketsecurity/lib-stable/releases/socket-btm'

import { errorMessage } from '../lib/error-utils.mts'
import { RELEASE_ASSETS_MANIFEST_PATH } from '../lib/paths.mts'
import { parseChecksums } from '../../../scripts/fleet/build/infra/lib/release-checksums/core.mts'

const logger = getDefaultLogger()

const __filename = fileURLToPath(import.meta.url)
const __dirname = path.dirname(__filename)
const packageRoot = path.join(__dirname, '..')

/**
 * Tools to sync checksums for (only packages with GitHub releases).
 */
const TOOLS = [
  'lief',
  'curl',
  'stubs',
  'binpress',
  'binflate',
  'binject',
  'boringssl',
  'node-smol',
  'ink',
  'iocraft',
  'onnxruntime',
  'models',
  'yoga-layout',
  'opentui',
]

/**
 * Fetch checksums for a tool from GitHub release.
 *
 * @param {string} tool - Tool name.
 *
 * @returns {Promise<
 *   { tag: string; checksums: Record<string, string> } | undefined
 * >}
 */
export async function fetchToolChecksums(tool) {
  const toolPrefix = `${tool}-`

  try {
    // Get latest release tag.
    const tag = await getLatestRelease(toolPrefix, SOCKET_BTM_REPO, {
      quiet: true,
    })
    if (!tag) {
      logger.warn(`No release found for ${tool}`)
      return undefined
    }

    // Download checksums.txt to temp location.
    const tempDir = path.join(packageRoot, 'build', 'temp')
    await safeMkdir(tempDir)
    const checksumPath = path.join(tempDir, `${tool}-checksums-${tag}.txt`)

    // Download checksums.txt.
    await downloadReleaseAsset(
      tag,
      'checksums.txt',
      checksumPath,
      SOCKET_BTM_REPO,
      { quiet: true },
    )

    // Parse checksums.
    const content = await fs.readFile(checksumPath, 'utf8')
    const checksums = parseChecksums(content)

    // Clean up temp file.
    await safeDelete(checksumPath).catch(() => {})

    return { __proto__: null, checksums, tag }
  } catch (e) {
    logger.warn(`Failed to fetch checksums for ${tool}: ${errorMessage(e)}`)
    return undefined
  }
}

type ReleaseAssetStatus = 'updated' | 'unchanged' | 'skipped' | 'failed'
type ReleaseAssetData = Record<string, unknown>

function readReleaseAssets(): ReleaseAssetData {
  if (!existsSync(RELEASE_ASSETS_MANIFEST_PATH)) {
    return { __proto__: null }
  }
  try {
    const currentData: unknown = JSON.parse(
      readFileSync(RELEASE_ASSETS_MANIFEST_PATH, 'utf8'),
    )
    if (isPlainObject(currentData)) {
      return currentData
    }
    logger.warn(
      `Failed to parse ${RELEASE_ASSETS_MANIFEST_PATH}: expected a JSON object.`,
    )
  } catch (error) {
    logger.warn(
      `Failed to parse ${RELEASE_ASSETS_MANIFEST_PATH}: ${errorMessage(error)}`,
    )
  }
  return { __proto__: null }
}

async function syncToolReleaseAssets(
  tool: string,
  currentData: ReleaseAssetData,
  options?: { force?: boolean | undefined } | undefined,
): Promise<ReleaseAssetStatus> {
  const opts = { __proto__: null, ...options } as typeof options
  const force = opts?.force ?? false
  logger.info(`Fetching checksums for ${tool}...`)
  const result = await fetchToolChecksums(tool)
  if (!result) {
    return 'skipped'
  }

  const { checksums, tag } = result
  const checksumCount = Object.keys(checksums).length
  if (checksumCount === 0) {
    logger.warn(`  No checksums found in release ${tag}`)
    return 'failed'
  }

  const currentValue = currentData[tool]
  const current = isPlainObject(currentValue) ? currentValue : undefined
  const currentTag = typeof current?.tag === 'string' ? current.tag : undefined
  const currentChecksums = isPlainObject(current?.checksums)
    ? current.checksums
    : {}
  const currentCount = Object.keys(currentChecksums).length
  const tagChanged = currentTag !== tag
  const checksumsChanged =
    JSON.stringify(checksums) !== JSON.stringify(currentChecksums)

  if (!force && !tagChanged && !checksumsChanged) {
    logger.info(`  ${tool}: unchanged (${tag}, ${checksumCount} checksums)`)
    return 'unchanged'
  }

  currentData[tool] = {
    ...current,
    description: releaseAssetDescription(current, tool),
    tag,
    checksums,
  }

  if (tagChanged) {
    logger.success(
      `  ${tool}: ${currentTag || '(none)'} -> ${tag} (${checksumCount} checksums)`,
    )
  } else {
    logger.success(
      `  ${tool}: updated checksums (${currentCount} -> ${checksumCount})`,
    )
  }
  return 'updated'
}

function releaseAssetDescription(
  current: Record<string, unknown> | undefined,
  tool: string,
): string {
  const description = current?.description
  if (typeof description === 'string' && description) {
    return description
  }
  return `${tool} binaries`
}

/**
 * Main sync function.
 */
// oxlint-disable-next-line eslint/complexity -- release update dispatcher
async function main() {
  const args = process.argv.slice(2)
  const force = args.includes('--force')
  const dryRun = args.includes('--dry-run')
  const toolArg = args.find(arg => arg.startsWith('--tool='))
  const toolFilter = toolArg ? toolArg.split('=')[1] : undefined

  // Validate tool filter.
  if (toolFilter && !TOOLS.includes(toolFilter)) {
    logger.fail(`Unknown tool: ${toolFilter}`)
    logger.info(`Valid tools: ${TOOLS.join(', ')}`)
    process.exitCode = 1
    return
  }

  const toolsToSync = toolFilter ? [toolFilter] : TOOLS

  const currentData = readReleaseAssets()

  logger.info(`Syncing checksums for ${toolsToSync.length} tool(s)...`)
  logger.info('')

  const totals: Record<ReleaseAssetStatus, number> = {
    updated: 0,
    unchanged: 0,
    skipped: 0,
    failed: 0,
  }

  for (let i = 0, { length } = toolsToSync; i < length; i += 1) {
    const tool = toolsToSync[i]!
    const status = await syncToolReleaseAssets(tool, currentData, { force })
    totals[status] += 1
  }

  logger.info('')

  // Write updated file.
  if (totals.updated > 0 && !dryRun) {
    const output = {
      ...currentData,
      $schema: './release-assets.schema.json',
      $comment:
        "SHA-256 checksums for GitHub release assets. Run 'pnpm --filter build-infra update-release-assets' to update.",
    }

    await fs.writeFile(
      RELEASE_ASSETS_MANIFEST_PATH,
      stringifyWithFormatting(output, getDefaultFormatting()),
      'utf8',
    )
    logger.success(`Updated ${RELEASE_ASSETS_MANIFEST_PATH}`)
  } else if (dryRun && totals.updated > 0) {
    logger.info('Dry run - no changes written')
  }

  // Summary.
  logger.info('')
  const parts = [`${totals.updated} updated`, `${totals.unchanged} unchanged`]
  if (totals.skipped > 0) {
    parts.push(`${totals.skipped} skipped (no release)`)
  }
  if (totals.failed > 0) {
    parts.push(`${totals.failed} failed`)
  }
  const summary = parts.join(', ')

  if (totals.failed > 0) {
    logger.warn(`Summary: ${summary}`)
    process.exitCode = 1
  } else {
    logger.success(`Summary: ${summary}`)
  }
}

main().catch(error => {
  logger.fail(`Sync failed: ${errorMessage(error)}`)
  process.exitCode = 1
})
