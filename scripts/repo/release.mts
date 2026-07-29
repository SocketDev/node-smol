/*
 * @file Repo release-assembly lane. Assembles GitHub release assets from a
 *   built output directory, writes a SHA-256 `checksums.txt` manifest, and
 *   cuts a DRAFT release via `gh release create --draft`. Honesty rules:
 *
 *   - DRY-RUN IS THE DEFAULT. Without `--publish` it prints the exact `gh release
 *     create` command, the computed per-asset digests, and every precondition —
 *     nothing is uploaded.
 *   - The repo has ZERO releases today and no binary build lane
 *     (`scripts/repo/build.mts --target binary` names what is missing), so
 *     until that lane is ported the asset directory must be populated by hand
 *     or by CI — this script fails loud when it is empty rather than cutting an
 *     assetless release.
 *   - The release is created as a DRAFT. Undrafting via `gh release edit <tag>
 *     --draft=false` is a separate deliberate act — fleet releases are
 *     immutable once public, and the github-release workflow's order rule
 *     applies: publish gates run before the final release marker exists. USAGE:
 *     node scripts/repo/release.mts --tag v0.1.0 [--dir build/release]
 *     [--notes-file <path>] [--publish]
 */

import crypto from 'node:crypto'
import {
  createReadStream,
  existsSync,
  readdirSync,
  writeFileSync,
} from 'node:fs'
import path from 'node:path'
import process from 'node:process'
import { pipeline } from 'node:stream/promises'
import { parseArgs } from 'node:util'

import { getDefaultLogger } from '@socketsecurity/lib-stable/logger/default'

import { isMainModule } from '../fleet/_shared/is-main-module.mts'
import { runCapture, runInherit } from '../fleet/publish-infra/shared.mts'
import { RELEASE_ASSETS_DIR, REPO_ROOT } from './paths.mts'

const logger = getDefaultLogger()

const CHECKSUMS_BASENAME = 'checksums.txt'

const TAG_PATTERN = /^v\d+\.\d+\.\d+(?:-[0-9A-Za-z.-]+)?$/

/**
 * Recursively list the release-asset files under `dir`, excluding a previously
 * generated checksums manifest — that file is regenerated every run. Returns
 * absolute paths, sorted for deterministic manifests.
 */
export function collectAssets(dir: string): string[] {
  const dirents = readdirSync(dir, { recursive: true, withFileTypes: true })
  const files: string[] = []
  for (let i = 0, { length } = dirents; i < length; i += 1) {
    const dirent = dirents[i]!
    if (!dirent.isFile() || dirent.name === CHECKSUMS_BASENAME) {
      continue
    }
    files.push(path.join(dirent.parentPath, dirent.name))
  }
  return files.toSorted()
}

/**
 * SHA-256 of one file, streamed (release binaries are tens of MB).
 */
export async function sha256File(file: string): Promise<string> {
  const hash = crypto.createHash('sha256')
  await pipeline(createReadStream(file), hash)
  return hash.digest('hex')
}

/**
 * Render the `checksums.txt` body — `shasum -a 256 -c` compatible
 * (`<hex><two spaces><basename>`), one line per asset, sorted by name.
 */
export function renderChecksums(
  digests: Array<{ digest: string; file: string }>,
): string {
  return `${digests
    .map(entry => `${entry.digest}  ${path.basename(entry.file)}`)
    .join('\n')}\n`
}

const EMPTY_DIR_MESSAGE = (dir: string) =>
  `release.mts: no release assets found — refusing to cut an empty release.
  What:  the assembly step needs built platform artifacts to upload.
  Where: ${dir}
  Saw:   directory missing or contains no files.
  Fix:   this repo has no binary build lane yet — run
         node scripts/repo/build.mts --target binary for exactly what is
         missing and where the socket-btm sources are. Once artifacts exist,
         place them under the directory above (or pass --dir) and re-run.`

export async function main(): Promise<void> {
  const { values } = parseArgs({
    allowPositionals: false,
    args: process.argv.slice(2),
    options: {
      dir: { type: 'string' },
      'notes-file': { type: 'string' },
      publish: { default: false, type: 'boolean' },
      tag: { type: 'string' },
    },
  })
  const publish = values.publish
  const dir = values.dir
    ? path.resolve(REPO_ROOT, values.dir)
    : RELEASE_ASSETS_DIR

  const tag = values.tag
  if (!tag || !TAG_PATTERN.test(tag)) {
    logger.error(
      `release.mts: --tag is required and must look like v1.2.3 (saw ${JSON.stringify(tag ?? '')}).\n` +
        '  Fix:   pass --tag vX.Y.Z (the USER names the version — never invent one).',
    )
    process.exitCode = 1
    return
  }

  if (!existsSync(dir)) {
    logger.error(EMPTY_DIR_MESSAGE(dir))
    process.exitCode = 1
    return
  }
  const assets = collectAssets(dir)
  if (assets.length === 0) {
    logger.error(EMPTY_DIR_MESSAGE(dir))
    process.exitCode = 1
    return
  }

  const digests: Array<{ digest: string; file: string }> = []
  for (let i = 0, { length } = assets; i < length; i += 1) {
    const file = assets[i]!
    // oxlint-disable-next-line no-await-in-loop -- sequential hashing keeps memory flat and the manifest ordering obvious.
    digests.push({ digest: await sha256File(file), file })
  }
  const checksumsPath = path.join(dir, CHECKSUMS_BASENAME)
  const checksumsBody = renderChecksums(digests)

  const notesFile = values['notes-file']
  if (notesFile && !existsSync(path.resolve(REPO_ROOT, notesFile))) {
    logger.error(
      `release.mts: --notes-file does not exist: ${notesFile}\n` +
        '  Fix:   pass a readable notes file, or omit for generated notes.',
    )
    process.exitCode = 1
    return
  }
  const notesArgs = notesFile
    ? ['--notes-file', path.resolve(REPO_ROOT, notesFile)]
    : [
        '--notes',
        `node-smol ${tag} — platform binaries plus a SHA-256 manifest (${CHECKSUMS_BASENAME}). Verify downloads with: shasum -a 256 -c ${CHECKSUMS_BASENAME}`,
      ]
  const ghArgs = [
    'release',
    'create',
    tag,
    '--draft',
    '--title',
    tag,
    ...notesArgs,
    ...assets,
    checksumsPath,
  ]

  logger.log(`assets (${assets.length}) from ${dir}:`)
  for (let i = 0, { length } = digests; i < length; i += 1) {
    const entry = digests[i]!
    logger.log(`  ${entry.digest}  ${path.relative(dir, entry.file)}`)
  }

  if (!publish) {
    logger.log('')
    logger.log(
      `plan (dry-run — pass --publish to execute): gh ${ghArgs.join(' ')}`,
    )
    logger.log(`  cwd:           ${REPO_ROOT}`)
    logger.log(
      `  would write:   ${checksumsPath} (${digests.length} line${digests.length === 1 ? '' : 's'})`,
    )
    logger.log(
      '  preconditions: gh CLI authenticated with repo write access;' +
        ` no existing release for ${tag} (verify: gh release view ${tag});` +
        ' the version was named by the user',
    )
    logger.log(
      `  follow-up:     gh release edit ${tag} --draft=false (deliberate, separate act — publishing makes the release immutable)`,
    )
    return
  }

  // Real cut. Verify state before acting (fleet rule): the gh CLI must be
  // authenticated, and the release must not already exist.
  const ghProbe = await runCapture('gh', ['--version'], REPO_ROOT)
  if (ghProbe.code !== 0) {
    logger.error('release.mts: gh CLI not found on PATH.')
    logger.error(
      '  Fix:   install the GitHub CLI from https://cli.github.com and run gh auth login.',
    )
    process.exitCode = 1
    return
  }
  const authProbe = await runCapture('gh', ['auth', 'status'], REPO_ROOT)
  if (authProbe.code !== 0) {
    logger.error('release.mts: gh CLI is not authenticated.')
    logger.error(
      '  Fix:   gh auth login — cutting releases needs repo write access.',
    )
    process.exitCode = 1
    return
  }
  const existing = await runCapture(
    'gh',
    ['release', 'view', tag, '--json', 'tagName,isDraft'],
    REPO_ROOT,
  )
  if (existing.code === 0) {
    logger.error(
      `release.mts: a release for ${tag} already exists — refusing to double-cut.\n` +
        `  Saw:   gh release view ${tag} resolved: ${existing.stdout.trim()}\n` +
        '  Fix:   pick a new version (the USER names it), or edit the existing\n' +
        '         draft via gh release upload / gh release edit.',
    )
    process.exitCode = 1
    return
  }

  writeFileSync(checksumsPath, checksumsBody)
  logger.log(`wrote ${checksumsPath}`)
  const code = await runInherit('gh', ghArgs, REPO_ROOT)
  if (code !== 0) {
    logger.error(`release.mts: gh release create exited ${code}`)
    process.exitCode = code
    return
  }
  logger.success(
    `draft release ${tag} created with ${assets.length + 1} assets`,
  )
  logger.log(
    `Next: gh release edit ${tag} --draft=false — a separate deliberate act; public releases are immutable.`,
  )
}

// Entrypoint-guarded: importing this module (unit tests of its exported
// helpers) must not execute the script.
if (isMainModule(import.meta.url)) {
  main().catch((e: unknown) => {
    logger.error(e)
    process.exitCode = 1
  })
}
