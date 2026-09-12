#!/usr/bin/env node
/**
 * Validate the C ABI map against its committed snapshot without source
 * discovery. Only --update reads the contained source after commit and checksum
 * verification.
 */

import crypto from 'node:crypto'
import { existsSync, readdirSync, readFileSync, writeFileSync } from 'node:fs'
import path from 'node:path'
import process from 'node:process'

import { isAgent } from '@socketsecurity/lib-stable/env/agents'
import { errorMessage } from '@socketsecurity/lib-stable/errors/message'
import { getDefaultLogger } from '@socketsecurity/lib-stable/logger/default'

import { isMainModule } from '../../fleet/process/is-main-module.mts'
import { runMain } from '../../fleet/process/run-main.mts'
import { repositoryContainsTarget } from '../../../.git-hooks/_shared/repo-containment.mts'
import {
  assertStuieSourcePath,
  readStuieSourcePin,
  verifyStuieSource,
} from './smol-tui-cabi-symbols-are-mapped/source.mts'
import { REPO_ROOT } from '../paths.mts'
import {
  CABI_SRC_SUBDIR,
  CHECK_REL_PATH,
  compareCabiSymbolName,
  extractCabiSymbols,
  LOG_PREFIX,
  MAP_REL_PATH,
  narrowSnapshotJson,
  narrowSymbolMapJson,
  readOpentuiPin,
  renderFindings,
  SNAPSHOT_REL_PATH,
  summarize,
  validateMap,
} from './smol-tui-cabi-symbols-are-mapped/audit.mts'
import type {
  CabiSymbol,
  CabiSymbolMap,
  CabiSymbolSnapshot,
} from './smol-tui-cabi-symbols-are-mapped/audit.mts'

export * from './smol-tui-cabi-symbols-are-mapped/audit.mts'

const logger = getDefaultLogger()

function reportSuccess(message: string): void {
  if (!isAgent()) {
    logger.success(message)
  }
}

/**
 * Compare the snapshot's symbol set against a live extraction from `stuieDir`.
 * Undefined when the two agree, otherwise a message naming the delta.
 */
export function describeSnapshotDrift(
  snapshot: CabiSymbolSnapshot,
  stuieDir: string,
): string | undefined {
  const live = new Set(extractCheckoutSymbols(stuieDir).map(sym => sym.name))
  const stored = new Set(snapshot.symbols.map(sym => sym.name))
  const added = [...live].filter(name => !stored.has(name))
  const gone = [...stored].filter(name => !live.has(name))
  if (added.length === 0 && gone.length === 0) {
    return undefined
  }
  return `snapshot drift vs ${stuieDir}: ${added.length} only in the checkout [${added.slice(0, 5).join(', ')}], ${gone.length} only in the snapshot [${gone.slice(0, 5).join(', ')}]. Run \`node ${CHECK_REL_PATH} --update\`.`
}

/**
 * Every C ABI symbol a stuie checkout currently exports, sorted by name.
 */
export function extractCheckoutSymbols(stuieDir: string): CabiSymbol[] {
  return extractSnapshotFromCheckout(stuieDir).symbols
}

/**
 * Build the committed snapshot from a stuie checkout.
 */
export function extractSnapshotFromCheckout(
  stuieDir: string,
): CabiSymbolSnapshot {
  assertStuieSourcePath(REPO_ROOT, stuieDir)
  const srcDir = path.join(stuieDir, CABI_SRC_SUBDIR)
  if (!repositoryContainsTarget(REPO_ROOT, srcDir)) {
    throw new TypeError('C ABI source directory escapes the repository.')
  }
  const names = readdirSync(srcDir)
    .filter(name => name.endsWith('.rs'))
    .toSorted()
  const hashes: Array<[string, string]> = []
  const symbols: CabiSymbol[] = []
  for (let i = 0, { length } = names; i < length; i += 1) {
    const name = names[i]!
    const rel = `${CABI_SRC_SUBDIR}/${name}`
    const file = path.join(srcDir, name)
    if (!repositoryContainsTarget(REPO_ROOT, file)) {
      throw new TypeError('C ABI source file escapes the repository.')
    }
    const text = readFileSync(file, 'utf8')
    hashes.push([rel, crypto.createHash('sha256').update(text).digest('hex')])
    symbols.push(...extractCabiSymbols(text, rel))
  }
  const gitmodulesPath = path.join(stuieDir, '.gitmodules')
  if (!repositoryContainsTarget(REPO_ROOT, gitmodulesPath)) {
    throw new TypeError('Source metadata escapes the repository.')
  }
  return {
    files: Object.fromEntries(hashes),
    opentuiPin: readOpentuiPin(
      existsSync(gitmodulesPath) ? readFileSync(gitmodulesPath, 'utf8') : '',
    ),
    stuieCommit: readStuieSourcePin(REPO_ROOT).ref,
    symbols: symbols.toSorted(compareCabiSymbolName),
  }
}

/**
 * Read a repo-relative file, or undefined when missing or unreadable.
 */
export function readRepoFileText(file: string): string | undefined {
  const absolute = path.join(REPO_ROOT, file)
  if (!repositoryContainsTarget(REPO_ROOT, absolute) || !existsSync(absolute)) {
    return undefined
  }
  try {
    return readFileSync(absolute, 'utf8')
  } catch {
    return undefined
  }
}

/**
 * Prove the gate can fail: a phantom snapshot symbol no map row covers must
 * come back as an `unmapped-symbol` finding. A checker that stays green here
 * is blind, and every later green run would mean nothing.
 */
export function runSelfTest(): boolean {
  const file = `${CABI_SRC_SUBDIR}/cabi.rs`
  const phantom = 'phantomSelfTestSymbol'
  const anchor = 'anchorSelfTestSymbol'
  const symbolOf = (name: string): CabiSymbol => ({
    file,
    name,
    signature: `pub extern "C" fn ${name}()`,
    unsafe: false,
  })
  const snapshot: CabiSymbolSnapshot = {
    files: {},
    opentuiPin: '',
    stuieCommit: '',
    symbols: [symbolOf(anchor), symbolOf(phantom)],
  }
  const map: CabiSymbolMap = {
    pendingCeiling: 0,
    rows: [
      {
        counterpart: {
          file: 'packages/tui-infra/src/selftest.cc',
          identifier: 'SelfTestAnchor',
        },
        status: 'ported',
        symbol: anchor,
      },
    ],
  }
  const findings = validateMap(snapshot, map, () => 'void SelfTestAnchor() {}')
  return findings.some(
    finding => finding.kind === 'unmapped-symbol' && finding.symbol === phantom,
  )
}

function containedCabiAuditPath(file: string): string {
  const target = path.join(REPO_ROOT, file)
  if (!repositoryContainsTarget(REPO_ROOT, target)) {
    throw new TypeError(
      'Audit input is outside the repository. Where: C ABI snapshot or map. Saw an external path, wanted a contained file. Fix: restore the repository audit inputs.',
    )
  }
  return target
}

export async function main(): Promise<void> {
  const argv = process.argv.slice(2)
  if (argv.includes('--self-test')) {
    if (runSelfTest()) {
      reportSuccess(`${LOG_PREFIX} self-test: the phantom symbol was caught.`)
      return
    }
    logger.fail(
      `${LOG_PREFIX} self-test: checker failed to fail — a phantom snapshot symbol with no map row went unreported.`,
    )
    process.exitCode = 1
    return
  }
  const snapshotPath = containedCabiAuditPath(SNAPSHOT_REL_PATH)
  const mapPath = containedCabiAuditPath(MAP_REL_PATH)
  if (argv.includes('--update')) {
    const stuieDir = await verifyStuieSource(REPO_ROOT)
    const snapshot = extractSnapshotFromCheckout(stuieDir)
    writeFileSync(snapshotPath, `${JSON.stringify(snapshot, undefined, 2)}\n`)
    reportSuccess(
      `${LOG_PREFIX} wrote ${SNAPSHOT_REL_PATH}: ${snapshot.symbols.length} symbols from stuie ${snapshot.stuieCommit.slice(0, 12)}.`,
    )
    return
  }
  if (!existsSync(snapshotPath)) {
    logger.fail(
      `${LOG_PREFIX} missing ${SNAPSHOT_REL_PATH} — it is generated, so run \`node ${CHECK_REL_PATH} --update\` after materializing the pinned stuie source.`,
    )
    process.exitCode = 1
    return
  }
  if (!existsSync(mapPath)) {
    logger.fail(
      `${LOG_PREFIX} missing ${MAP_REL_PATH} — it is hand-curated, so add one row per snapshot symbol with status ported, out-of-scope, or pending.`,
    )
    process.exitCode = 1
    return
  }
  let snapshot: CabiSymbolSnapshot | undefined
  let map: CabiSymbolMap | undefined
  try {
    const rawSnapshot: unknown = JSON.parse(readFileSync(snapshotPath, 'utf8'))
    const rawMap: unknown = JSON.parse(readFileSync(mapPath, 'utf8'))
    snapshot = narrowSnapshotJson(rawSnapshot)
    map = narrowSymbolMapJson(rawMap)
  } catch (e) {
    logger.fail(
      `${LOG_PREFIX} could not read the audit inputs: ${errorMessage(e)}`,
    )
    process.exitCode = 1
    return
  }
  if (!snapshot || !map) {
    logger.fail(
      `${LOG_PREFIX} ${SNAPSHOT_REL_PATH} or ${MAP_REL_PATH} has the wrong shape: the snapshot needs a symbols array, the map needs pendingCeiling plus rows.`,
    )
    process.exitCode = 1
    return
  }
  const findings = validateMap(snapshot, map, readRepoFileText)
  const pin = readStuieSourcePin(REPO_ROOT)
  const drift =
    snapshot.stuieCommit === pin.ref
      ? undefined
      : 'Snapshot commit differs from the pinned source. Run the snapshot update after materializing the declared revision.'
  if (findings.length || drift) {
    const summary = summarize(findings)
    logger.fail(
      [
        `${LOG_PREFIX} the stuie-cabi symbol map is incomplete or unverifiable:`,
        ...(drift ? ['', `    - ${drift}`] : []),
        ...(summary.total ? ['', renderFindings(findings)] : []),
        '',
        `  ${summary.total} finding(s) across ${summary.byKind.size} class(es). Curate ${MAP_REL_PATH}.`,
      ].join('\n'),
    )
    process.exitCode = 1
    return
  }
  reportSuccess(
    `${LOG_PREFIX} all ${snapshot.symbols.length} stuie-cabi symbols are mapped across ${map.rows.length} rows.`,
  )
}

if (isMainModule(import.meta.url)) {
  runMain(main, {
    describe: 'Verify the committed C ABI symbol snapshot.',
    help: 'Usage: smol-tui-cabi-symbols-are-mapped [--update|--self-test]',
  })
}
