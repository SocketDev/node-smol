/*
 * @file Repo build lane entry. `node scripts/repo/build.mts` orchestrates what
 *   this repo can actually build TODAY, and refuses loudly to pretend about
 *   the rest.
 *   Targets:
 *
 *   - `--target base` (default) — REAL. Bakes the compile-environment image(s)
 *     declared in `.config/repo/socket-wheelhouse.json` `docker.prebakes` via
 *     `docker buildx build`, exactly like the prebake-publish workflow
 *     (.github/workflows/prebake-publish.yml) does on dispatch. Local default
 *     builds the host platform only and `--load`s it into the docker daemon;
 *     `--push` builds every declared platform and pushes to GHCR (needs a
 *     `docker login ghcr.io` with packages:write — the workflow dispatch is the
 *     canonical publisher, this flag exists for break-glass use).
 *   - `--target binary` — NOT PORTED YET. The node-smol binary build (Node source
 *     checkout + Socket patch series + builtins + SEA packaging +
 *     strip/compress) still lives in socket-btm's history; see the fail-loud
 *     message below for exactly what is missing and where the sources are.
 *     `--dry-run` prints the exact commands with their preconditions instead of
 *     running them. USAGE: node scripts/repo/build.mts [--target base|binary]
 *     [--dry-run] [--push] [--platforms linux/arm64,...]
 */

import { existsSync } from 'node:fs'
import os from 'node:os'
import path from 'node:path'
import process from 'node:process'
import { parseArgs } from 'node:util'

import { getDefaultLogger } from '@socketsecurity/lib-stable/logger/default'

import { isMainModule } from '../fleet/_shared/is-main-module.mts'
import { runCapture, runInherit } from '../fleet/registry-infra/shared.mts'
import { loadSocketWheelhouseConfig, REPO_ROOT } from './paths.mts'

const logger = getDefaultLogger()

/**
 * One `docker.prebakes.prebakes[]` entry, narrowed from the untyped config
 * (the fleet loader validates "JSON object" only — schema validation is
 * wheelhouse-side).
 */
export interface PrebakeEntry {
  dockerfile: string
  name: string
  platforms: string[]
}

export interface PrebakePlan {
  entries: PrebakeEntry[]
  registry: string
}

/**
 * Type guard for a plain JSON object — keeps the config walk free of type
 * assertions.
 */
function isRecord(value: unknown): value is Record<string, unknown> {
  return !!value && typeof value === 'object' && !Array.isArray(value)
}

/**
 * Extract the prebake plan from a parsed socket-wheelhouse.json root. Returns
 * an error string (for fail-loud reporting) when the shape is not usable.
 */
export function extractPrebakePlan(
  configValue: Record<string, unknown>,
): PrebakePlan | string {
  const docker = configValue['docker']
  if (!isRecord(docker)) {
    return 'config has no docker section'
  }
  const prebakes = docker['prebakes']
  if (!isRecord(prebakes)) {
    return 'config has no docker.prebakes section'
  }
  const registry = prebakes['registry']
  if (typeof registry !== 'string' || !registry) {
    return 'docker.prebakes.registry is missing'
  }
  const rawEntries = prebakes['prebakes']
  if (!Array.isArray(rawEntries) || rawEntries.length === 0) {
    return 'docker.prebakes.prebakes declares no images'
  }
  const entries: PrebakeEntry[] = []
  for (let i = 0, { length } = rawEntries; i < length; i += 1) {
    const entry: unknown = rawEntries[i]
    if (!isRecord(entry)) {
      return `docker.prebakes.prebakes[${i}] is not an object`
    }
    const name = entry['name']
    const dockerfile = entry['dockerfile']
    if (typeof name !== 'string' || typeof dockerfile !== 'string') {
      return `docker.prebakes.prebakes[${i}] is missing name/dockerfile`
    }
    const rawPlatforms = entry['platforms']
    const platforms = Array.isArray(rawPlatforms)
      ? rawPlatforms.filter((p): p is string => typeof p === 'string')
      : ['linux/amd64', 'linux/arm64']
    entries.push({ dockerfile, name, platforms })
  }
  return { entries, registry }
}

/**
 * The docker platform string for the machine we are on — what a no-`--push`
 * build can actually `--load` into the local daemon (buildx refuses to
 * `--load` a multi-platform result).
 */
export function hostDockerPlatform(): string {
  return os.arch() === 'arm64' ? 'linux/arm64' : 'linux/amd64'
}

export interface BuildxArgsOptions {
  push?: boolean | undefined
}

/**
 * Build the exact `docker buildx build` argv for one prebake entry — shared
 * between dry-run printing and real execution so the printed plan can never
 * drift from what runs.
 */
export function buildxArgs(
  entry: PrebakeEntry,
  registry: string,
  platforms: string,
  options?: BuildxArgsOptions | undefined,
): string[] {
  const opts = { __proto__: null, ...options } as BuildxArgsOptions
  const push = opts.push ?? false
  return [
    'buildx',
    'build',
    '-f',
    entry.dockerfile,
    '-t',
    `${registry}/${entry.name}:latest`,
    '--platform',
    platforms,
    push ? '--push' : '--load',
    '.',
  ]
}

const BINARY_LANE_MISSING = `build.mts --target binary: no binary lane exists in this repo yet — refusing to fake one.
  What:  the node-smol binary build (pinned Node source checkout, Socket patch
         series, builtins/additions tree, SEA packaging, strip + compress,
         platform artifact naming) has not been extracted into this repo.
  Where: SocketDev/socket-btm .config/repo/node-smol-rust-extraction.json
         marks packages/node-smol-builder and packages/npm as destined for
         node-smol with removalStatus "removed" — the sources were dropped
         from socket-btm (refactor(btm)!, commit 40b7e82a) before landing
         here. The pre-drop tree is socket-btm@55f8f23a
         packages/node-smol-builder (entry: scripts/common/shared/build.mts).
  Saw:   this repo carries no Node source pin, no Node patch series (patches/
         holds only a pnpm tool patch), no additions/ builtins tree, and no
         SEA/strip/compress scripts. Only the compile-environment image
         (--target base) is buildable today.
  Fix:   port the builder from socket-btm@55f8f23a packages/node-smol-builder,
         then wire this target to its entry script. Track the port before
         cutting any GitHub release (scripts/repo/release.mts).`

interface BaseTargetOptions {
  dryRun?: boolean | undefined
  platformsOverride?: string | undefined
  push?: boolean | undefined
}

/**
 * Bake the declared prebake images. Returns the process exit code.
 */
async function runBaseTarget(
  options?: BaseTargetOptions | undefined,
): Promise<number> {
  const opts = { __proto__: null, ...options } as BaseTargetOptions
  const dryRun = opts.dryRun ?? false
  const { platformsOverride } = opts
  const push = opts.push ?? false
  const loaded = loadSocketWheelhouseConfig(REPO_ROOT)
  if (!loaded) {
    logger.error(
      'build.mts: cannot load .config/repo/socket-wheelhouse.json.\n' +
        '  What:  the prebake declarations (registry, dockerfile, platforms) live there.\n' +
        '  Where: .config/repo/socket-wheelhouse.json (single source of truth,\n' +
        '         shared with .github/workflows/prebake-publish.yml).\n' +
        '  Saw:   file absent or unparseable.\n' +
        '  Fix:   restore the config; do not hand-inline image names here.',
    )
    return 1
  }
  const plan = extractPrebakePlan(loaded.value)
  if (typeof plan === 'string') {
    logger.error(
      `build.mts: unusable prebake config — ${plan}.\n` +
        `  Where: ${loaded.location.path}\n` +
        '  Fix:   repair the docker.prebakes section to match the wheelhouse schema.',
    )
    return 1
  }

  for (let i = 0, { length } = plan.entries; i < length; i += 1) {
    const entry = plan.entries[i]!
    const dockerfileAbs = path.join(REPO_ROOT, entry.dockerfile)
    if (!existsSync(dockerfileAbs)) {
      logger.error(
        `build.mts: declared Dockerfile is missing — ${entry.dockerfile}.\n` +
          `  Where: ${loaded.location.path} entry "${entry.name}"\n` +
          '  Fix:   restore the Dockerfile or fix the declaration.',
      )
      return 1
    }
    const platforms =
      platformsOverride ??
      (push ? entry.platforms.join(',') : hostDockerPlatform())
    if (!push && platforms.includes(',')) {
      logger.error(
        'build.mts: buildx cannot --load a multi-platform build.\n' +
          `  Saw:   --platforms ${platforms} without --push.\n` +
          '  Fix:   pass a single platform, or add --push (needs docker login\n' +
          '         ghcr.io with packages:write).',
      )
      return 1
    }
    const args = buildxArgs(entry, plan.registry, platforms, { push })
    if (dryRun) {
      logger.log(`plan [${entry.name}]: docker ${args.join(' ')}`)
      logger.log(`  cwd:           ${REPO_ROOT}`)
      logger.log(
        '  preconditions: docker CLI + buildx plugin installed;' +
          ' the FROM image (FLEET_BASE arg default,' +
          ' ghcr.io/socketdev/socket-wheelhouse/c-base:latest) must be pullable',
      )
      if (push) {
        logger.log(
          '                 --push additionally needs docker login ghcr.io with' +
            ' packages:write (canonical publisher: dispatch' +
            ' .github/workflows/prebake-publish.yml instead)',
        )
      }
      continue
    }
    const dockerProbe = await runCapture('docker', ['--version'], REPO_ROOT)
    if (dockerProbe.code !== 0) {
      logger.error(
        'build.mts: docker CLI not found on PATH.\n' +
          '  Fix:   install Docker (or run the bake in CI by dispatching\n' +
          '         .github/workflows/prebake-publish.yml).',
      )
      return 1
    }
    const buildxProbe = await runCapture(
      'docker',
      ['buildx', 'version'],
      REPO_ROOT,
    )
    if (buildxProbe.code !== 0) {
      logger.error(
        'build.mts: docker buildx plugin not available.\n' +
          '  Fix:   install the buildx plugin (ships with Docker Desktop;\n' +
          '         docker-buildx-plugin on Linux).',
      )
      return 1
    }
    logger.log(`baking ${entry.name} (${platforms})…`)
    const code = await runInherit('docker', args, REPO_ROOT)
    if (code !== 0) {
      logger.error(`build.mts: docker buildx exited ${code} for ${entry.name}`)
      return code
    }
    logger.success(`built ${plan.registry}/${entry.name}:latest`)
  }
  return 0
}

export async function main(): Promise<void> {
  const { values } = parseArgs({
    allowPositionals: false,
    args: process.argv.slice(2),
    options: {
      'dry-run': { default: false, type: 'boolean' },
      platforms: { type: 'string' },
      push: { default: false, type: 'boolean' },
      target: { default: 'base', type: 'string' },
    },
  })
  const target = values.target
  if (target === 'binary') {
    logger.error(BINARY_LANE_MISSING)
    process.exitCode = 1
    return
  }
  if (target !== 'base') {
    logger.error(
      `build.mts: unknown --target ${JSON.stringify(target)} — valid targets: base, binary.`,
    )
    process.exitCode = 1
    return
  }
  process.exitCode = await runBaseTarget({
    dryRun: values['dry-run'],
    platformsOverride: values.platforms,
    push: values.push,
  })
}

// Entrypoint-guarded: importing this module (unit tests of its exported
// helpers) must not execute the script.
if (isMainModule(import.meta.url)) {
  main().catch((e: unknown) => {
    logger.error(e)
    process.exitCode = 1
  })
}
