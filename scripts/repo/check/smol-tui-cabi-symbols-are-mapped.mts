#!/usr/bin/env node
/*
 * @file Repo check — every `extern "C"` symbol stuie's `stuie-cabi` crate
 *   exports is accounted for by `packages/tui-infra/cabi-symbol-map.json`:
 *   ported with verifiable evidence, explicitly out of scope, or explicitly
 *   pending under a named plan. The committed snapshot
 *   `packages/tui-infra/cabi-symbols.snapshot.json` is the hermetic input, so
 *   CI needs neither a stuie checkout nor the network. Modes: argless
 *   validates the map against the snapshot and, when a stuie checkout is
 *   reachable, also re-extracts live and fails on drift — that argless path is
 *   what `scripts/fleet/check.mts` invokes, so it writes nothing. `--update`
 *   re-extracts from the checkout and rewrites the snapshot; it is the only
 *   mode that writes. `--self-test` proves the gate can fail by feeding it a
 *   phantom symbol no map row covers. The pure extractor and validator live in
 *   `./smol-tui-cabi-symbols-are-mapped/audit.mts`, re-exported here so tests
 *   and callers have one import path.
 */

import crypto from 'node:crypto'
import { existsSync, readdirSync, readFileSync, writeFileSync } from 'node:fs'
import path from 'node:path'
import process from 'node:process'

import { errorMessage } from '@socketsecurity/lib-stable/errors/message'
import { getDefaultLogger } from '@socketsecurity/lib-stable/logger/default'
import { spawnSync } from '@socketsecurity/lib-stable/process/spawn/child'

import { isMainModule } from '../../fleet/_shared/is-main-module.mts'
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
  const srcDir = path.join(stuieDir, CABI_SRC_SUBDIR)
  const names = readdirSync(srcDir)
    .filter(name => name.endsWith('.rs'))
    .toSorted()
  const hashes: Array<[string, string]> = []
  const symbols: CabiSymbol[] = []
  for (let i = 0, { length } = names; i < length; i += 1) {
    const name = names[i]!
    const rel = `${CABI_SRC_SUBDIR}/${name}`
    const text = readFileSync(path.join(srcDir, name), 'utf8')
    hashes.push([rel, crypto.createHash('sha256').update(text).digest('hex')])
    symbols.push(...extractCabiSymbols(text, rel))
  }
  const gitmodulesPath = path.join(stuieDir, '.gitmodules')
  return {
    files: Object.fromEntries(hashes),
    opentuiPin: readOpentuiPin(
      existsSync(gitmodulesPath) ? readFileSync(gitmodulesPath, 'utf8') : '',
    ),
    stuieCommit: readGitHead(stuieDir),
    symbols: symbols.toSorted(compareCabiSymbolName),
  }
}

/**
 * HEAD of the stuie checkout, or an empty string when git cannot answer.
 */
export function readGitHead(stuieDir: string): string {
  // Main() is a sync CLI writer; HEAD must resolve before the snapshot is
  // serialized.
  // oxlint-disable-next-line socket/prefer-async-spawn -- see note above
  const result = spawnSync('git', ['-C', stuieDir, 'rev-parse', 'HEAD'], {
    encoding: 'utf8',
  })
  return result.status === 0 && typeof result.stdout === 'string'
    ? result.stdout.trim()
    : ''
}

/**
 * Read a repo-relative file, or undefined when missing or unreadable.
 */
export function readRepoFileText(file: string): string | undefined {
  const absolute = path.join(REPO_ROOT, file)
  if (!existsSync(absolute)) {
    return undefined
  }
  try {
    return readFileSync(absolute, 'utf8')
  } catch {
    return undefined
  }
}

/**
 * The stuie checkout to extract from, or undefined when none is reachable.
 */
export function resolveStuieDir(): string | undefined {
  const candidate =
    process.env['STUIE_DIR'] ?? path.join(REPO_ROOT, '..', 'stuie')
  return existsSync(path.join(candidate, CABI_SRC_SUBDIR))
    ? candidate
    : undefined
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

function main(): void {
  const argv = process.argv.slice(2)
  if (argv.includes('--self-test')) {
    if (runSelfTest()) {
      logger.success(`${LOG_PREFIX} self-test: the phantom symbol was caught.`)
      return
    }
    logger.fail(
      `${LOG_PREFIX} self-test: checker failed to fail — a phantom snapshot symbol with no map row went unreported.`,
    )
    process.exitCode = 1
    return
  }
  const stuieDir = resolveStuieDir()
  if (argv.includes('--update')) {
    if (!stuieDir) {
      logger.fail(
        `${LOG_PREFIX} --update needs a stuie checkout; set STUIE_DIR or clone stuie to ${path.join(REPO_ROOT, '..', 'stuie')}.`,
      )
      process.exitCode = 1
      return
    }
    const snapshot = extractSnapshotFromCheckout(stuieDir)
    writeFileSync(
      path.join(REPO_ROOT, SNAPSHOT_REL_PATH),
      `${JSON.stringify(snapshot, undefined, 2)}\n`,
    )
    logger.success(
      `${LOG_PREFIX} wrote ${SNAPSHOT_REL_PATH}: ${snapshot.symbols.length} symbols from stuie ${snapshot.stuieCommit.slice(0, 12)}.`,
    )
    return
  }
  const snapshotPath = path.join(REPO_ROOT, SNAPSHOT_REL_PATH)
  if (!existsSync(snapshotPath)) {
    logger.fail(
      `${LOG_PREFIX} missing ${SNAPSHOT_REL_PATH} — it is generated, so run \`node ${CHECK_REL_PATH} --update\` against a stuie checkout.`,
    )
    process.exitCode = 1
    return
  }
  const mapPath = path.join(REPO_ROOT, MAP_REL_PATH)
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
  let drift: string | undefined
  if (stuieDir) {
    drift = describeSnapshotDrift(snapshot, stuieDir)
  } else {
    logger.warn(
      `${LOG_PREFIX} DRIFT LEG SKIPPED — no stuie checkout at STUIE_DIR or ${path.join(REPO_ROOT, '..', 'stuie')}, so the snapshot was validated but not re-derived from stuie.`,
    )
  }
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
  logger.success(
    `${LOG_PREFIX} all ${snapshot.symbols.length} stuie-cabi symbols are mapped across ${map.rows.length} rows.`,
  )
}

if (isMainModule(import.meta.url)) {
  main()
}
