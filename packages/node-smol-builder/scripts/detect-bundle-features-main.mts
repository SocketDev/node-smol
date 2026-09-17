#!/usr/bin/env node
/**
 * @file Entrypoint for the detect-bundle-features CLI.
 *   Drives the feature-detection pipeline for a consumer's SEA bundle and
 *   emits either a JSON manifest or a human-readable report. Split from
 *   detect-bundle-features.mts to keep each file under the 500-line soft cap.
 */

import { existsSync, promises as fs } from 'node:fs'
import path from 'node:path'
import process from 'node:process'
import { pathToFileURL } from 'node:url'

import { getDefaultLogger } from '@socketsecurity/lib-stable/logger/default'
import { parseArgs } from '@socketsecurity/lib-stable/exe/argv/parse'

import { errorMessage } from 'local-build-infra/lib/error-utils'

import { SMOL_FEATURES } from './lib/smol-features.mts'
import { detectBundleFeatures } from './detect-bundle-features.mts'

// `parseArgs` widens every value to the union of all declared option types, so
// a string option still reads as string-or-boolean. Narrow once, here.
function stringArg(value: unknown): string | undefined {
  return typeof value === 'string' ? value : undefined
}

const logger = getDefaultLogger()

export function replacerStripProto(_key: string, value: unknown): unknown {
  return value
}

export function pathToFileURLString(p: string | undefined): string {
  if (!p) {
    return ''
  }
  return pathToFileURL(path.resolve(p)).href
}

async function main(): Promise<void> {
  async function readOverrides(overridesPath: string | undefined) {
    if (!overridesPath) {
      return undefined
    }
    try {
      const pkg = JSON.parse(await fs.readFile(overridesPath, 'utf8'))
      return pkg?.smol
        ? { __proto__: null, drop: pkg.smol.drop, keep: pkg.smol.keep }
        : undefined
    } catch (e) {
      logger.warn(
        `could not read overrides from ${overridesPath}: ${errorMessage(e)}`,
      )
      return undefined
    }
  }

  function printHumanReport(manifest, bundlePath: string) {
    logger.log(`Bundle: ${bundlePath}`)
    logger.log(`Hash:   ${manifest.bundleHash}`)
    logger.log('')
    logger.log('Feature          Use    Drop  Reason')
    logger.log('───────────────  ─────  ────  ──────')
    for (const feature of SMOL_FEATURES) {
      const result = manifest.features[feature.name]!
      logger.log(
        `${feature.name.padEnd(15)}  ${result.use.padEnd(5)}  ${(result.drop ? 'yes' : 'no').padEnd(4)}  ${result.reason}`,
      )
    }
    const dropped = SMOL_FEATURES.filter(
      feature => manifest.features[feature.name]!.drop,
    )
    const savedMb = dropped.reduce(
      (sum, feature) => sum + feature.approxBinaryMb,
      0,
    )
    logger.log('')
    logger.log(
      `Dropping ${dropped.length} feature(s) — est. ~${savedMb.toFixed(1)}MB binary reduction`,
    )
    logger.log(
      `V8-lite: ${manifest.v8Lite.recommended ? 'RECOMMENDED' : 'no'} — ${manifest.v8Lite.reason}`,
    )
    if (manifest.ambiguous.length) {
      logger.warn(
        `Ambiguous (kept conservatively): ${manifest.ambiguous.join(', ')}`,
      )
    }
    logger.log('')
    logger.log(
      `Configure flags: ${manifest.configureFlags.join(' ') || '(none)'}`,
    )
  }

  // pnpm forwards a caller's bare `--` separator into argv, and parseArgs
  // treats it as end-of-options. Drop a leading `--` so both the direct
  // `node scripts/…` form and the `pnpm run detect` form behave the same.
  const rawArgs = process.argv.slice(2)
  const args = rawArgs[0] === '--' ? rawArgs.slice(1) : rawArgs
  const { values } = parseArgs({
    args,
    options: {
      bundle: { type: 'string' },
      vfs: { type: 'string' },
      overrides: { type: 'string' },
      'v8-lite': { type: 'boolean' },
      json: { type: 'boolean' },
    },
    strict: false,
  })

  const bundlePath = stringArg(values['bundle'])
  if (!bundlePath || !existsSync(bundlePath)) {
    logger.fail(
      `--bundle is required and must exist (got: ${bundlePath ?? '<none>'})`,
    )
    process.exitCode = 1
    return
  }
  const vfsPath = stringArg(values['vfs'])
  if (vfsPath && !existsSync(vfsPath)) {
    logger.fail(`--vfs path does not exist: ${vfsPath}`)
    process.exitCode = 1
    return
  }

  const overridesPath = stringArg(values['overrides'])
  const overrides = await readOverrides(overridesPath)

  const manifest = await detectBundleFeatures({
    bundlePath,
    vfsPath,
    overrides,
  })

  // The operator may force the V8-lite recommendation into the emitted flags.
  if (values['v8-lite'] && manifest.v8Lite.recommended) {
    manifest.configureFlags.push('--v8-lite-mode')
  }

  if (values['json']) {
    logger.log(JSON.stringify(manifest, replacerStripProto, 2))
    return
  }

  printHumanReport(manifest, bundlePath)
}

// Run as a script (not when imported by tests).
if (import.meta.url === pathToFileURLString(process.argv[1])) {
  main().catch((e: unknown) => {
    logger.fail(errorMessage(e))
    process.exitCode = 1
  })
}
