/**
 * @file Seeded-manifest invariants. node-smol's tracked surface today is the
 *   seeded repo manifest (package.json) plus the fleet scaffolding it wires
 *   up; these tests pin the invariants that break installs or CI when they
 *   drift: every script entry must point at a file that exists, every
 *   devDependency must ride the `catalog:` protocol with a pinned entry in
 *   pnpm-workspace.yaml's catalog, and the pinned runtime (.node-version) must
 *   satisfy the declared engines floor. Catalog parsing is line-regex on the
 *   raw YAML — the same fleet-sanctioned approach as
 *   scripts/fleet/soak-rules.mts (no yaml dep at the repo root).
 */
import { existsSync, readFileSync } from 'node:fs'
import path from 'node:path'
import { fileURLToPath } from 'node:url'

import semver from 'semver'
import { describe, expect, it } from 'vitest'

const repoRoot = path.resolve(
  path.dirname(fileURLToPath(import.meta.url)),
  '..',
  '..',
  '..',
)

interface Manifest {
  devDependencies?: Record<string, string> | undefined
  devEngines?:
    | {
        packageManager?:
          | { name?: string | undefined; version?: string | undefined }
          | undefined
      }
    | undefined
  engines?: Record<string, string> | undefined
  name?: string | undefined
  private?: boolean | undefined
  scripts?: Record<string, string> | undefined
}

const manifest = JSON.parse(
  readFileSync(path.join(repoRoot, 'package.json'), 'utf8'),
) as Manifest

/**
 * The package names pinned in pnpm-workspace.yaml's top-level `catalog:`
 * block. Entries are `  'name': <spec>` (or unquoted) lines indented under
 * `catalog:` until the next top-level key.
 */
function readCatalogNames(yamlText: string): Set<string> {
  const names = new Set<string>()
  let inCatalog = false
  for (const line of yamlText.split(/\r?\n/)) {
    if (/^catalog:\s*$/.test(line)) {
      inCatalog = true
      continue
    }
    if (inCatalog && /^\S/.test(line)) {
      break
    }
    if (!inCatalog) {
      continue
    }
    const entry = /^ {2}'?([^']+?)'?:\s*\S/.exec(line)
    if (entry?.[1]) {
      names.add(entry[1])
    }
  }
  return names
}

describe('seeded manifest (package.json)', () => {
  it('declares the repo identity', () => {
    expect(manifest.name).toBe('node-smol')
    expect(manifest.private).toBe(true)
  })

  it('every node-invoking script points at a file that exists', () => {
    const scripts = manifest.scripts ?? {}
    const scriptNames = Object.keys(scripts)
    expect(scriptNames.length).toBeGreaterThan(0)
    for (const name of scriptNames) {
      const command = scripts[name]!
      // Match each `node <path>` invocation in the (possibly &&-chained)
      // command; non-node scripts (none today) are skipped, not failed.
      for (const match of command.matchAll(/(?:^|&&\s*)node\s+(\S+)/g)) {
        const scriptPath = match[1]!
        // A leading dash is a node flag (`node -e "<inline js>"`), not a
        // script file on disk.
        if (scriptPath.startsWith('-')) {
          continue
        }
        expect(
          existsSync(path.join(repoRoot, scriptPath)),
          `scripts.${name} references ${scriptPath}, which does not exist`,
        ).toBe(true)
      }
    }
  })

  it('every devDependency rides the catalog: protocol', () => {
    const devDeps = manifest.devDependencies ?? {}
    const names = Object.keys(devDeps)
    expect(names.length).toBeGreaterThan(0)
    for (const name of names) {
      expect(
        devDeps[name],
        `devDependencies.${name} must use the catalog: protocol`,
      ).toBe('catalog:')
    }
  })

  it('every devDependency has a pinned entry in the workspace catalog', () => {
    const catalogNames = readCatalogNames(
      readFileSync(path.join(repoRoot, 'pnpm-workspace.yaml'), 'utf8'),
    )
    expect(catalogNames.size).toBeGreaterThan(0)
    const missing = Object.keys(manifest.devDependencies ?? {}).filter(
      name => !catalogNames.has(name),
    )
    expect(
      missing,
      'devDependencies missing from the pnpm-workspace.yaml catalog',
    ).toEqual([])
  })

  it('the pinned runtime (.node-version) satisfies the engines.node floor', () => {
    const pinned = readFileSync(
      path.join(repoRoot, '.node-version'),
      'utf8',
    ).trim()
    expect(semver.valid(pinned), '.node-version must be an exact version').toBe(
      pinned,
    )
    const enginesNode = manifest.engines?.['node']
    expect(enginesNode).toBeTruthy()
    expect(
      semver.satisfies(pinned, enginesNode!),
      `.node-version ${pinned} must satisfy engines.node ${enginesNode}`,
    ).toBe(true)
  })

  it('the package-manager pins are coherent (pnpm, engines floor within devEngines range)', () => {
    const packageManager = manifest.devEngines?.packageManager
    expect(packageManager?.name).toBe('pnpm')
    const devEnginesRange = packageManager?.version
    const enginesFloor = manifest.engines?.['pnpm']
    expect(devEnginesRange).toBeTruthy()
    expect(enginesFloor).toBeTruthy()
    const floorVersion = semver.minVersion(enginesFloor!)
    expect(floorVersion).toBeTruthy()
    expect(
      semver.satisfies(floorVersion!, devEnginesRange!),
      `engines.pnpm floor ${enginesFloor} must sit inside devEngines range ${devEnginesRange}`,
    ).toBe(true)
  })
})
