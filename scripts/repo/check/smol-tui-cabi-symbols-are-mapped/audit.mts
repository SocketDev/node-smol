/*
 * @file Pure core of the `smol-tui-cabi-symbols-are-mapped` check: the C ABI
 *   symbol extractor, the map validator, and the finding vocabulary they share.
 *   Nothing here touches the filesystem, git, or a logger — every function is
 *   pure over its arguments, with counterpart-file reads injected as a reader
 *   callback, so the whole fail-class surface is unit testable. The CLI shell
 *   that supplies the real filesystem lives one level up in
 *   `../smol-tui-cabi-symbols-are-mapped.mts`, which re-exports this module.
 */

import { normalizePath } from '@socketsecurity/lib-stable/paths/normalize'
import { escapeRegExp } from '@socketsecurity/lib-stable/regexps/escape'

// Repo-relative dirs a `ported` counterpart may cite. Evidence outside these
// roots is not part of the `node:smol-tui` surface, so the claim would be
// unverifiable by this check.
export const ALLOWED_COUNTERPART_ROOTS: readonly string[] = [
  'packages/node-smol-builder/additions/source-patched/',
  'packages/tui-infra/',
]

// Where the C ABI surface lives inside a stuie checkout.
export const CABI_SRC_SUBDIR = 'crates/stuie-cabi/src'

// The check's own path, quoted back to the operator in fix hints.
export const CHECK_REL_PATH =
  'scripts/repo/check/smol-tui-cabi-symbols-are-mapped.mts'

export const LOG_PREFIX = '[smol-tui-cabi-symbols-are-mapped]'

export const MAP_REL_PATH = 'packages/tui-infra/cabi-symbol-map.json'

export const SNAPSHOT_REL_PATH = 'packages/tui-infra/cabi-symbols.snapshot.json'

// A C ABI export's declaration HEADER, anchored at the start of a line so a
// `///` doc line quoting one never matches:
//   `^[ \t]*`               leading indentation and nothing else
//   `pub\s+`                the visibility keyword
//   `(unsafe\s+)?`          group 1, present when the fn is declared unsafe
//   `extern\s+"C"\s+fn\s+`  the C ABI marker
//   `([A-Za-z_]\w*)`        group 2, the exported symbol name
//   `\s*\(`                 the opening paren; the paren scan takes over there
const CABI_FN_HEADER_PATTERN =
  '^[ \\t]*pub\\s+(unsafe\\s+)?extern\\s+"C"\\s+fn\\s+([A-Za-z_]\\w*)\\s*\\('

// The commit an opentui submodule block pins in stuie's `.gitmodules`:
//   `\[submodule "[^"]*opentui[^"]*"\]`  the block header naming opentui
//   `[\s\S]*?`                           a lazy hop over the block's other keys
//   `ref\s*=\s*`                         the pin key
//   `([0-9a-f]{7,40})`                   group 1, the pinned commit sha
const OPENTUI_REF_RE =
  /\[submodule "[^"]*opentui[^"]*"\][\s\S]*?ref\s*=\s*([0-9a-f]{7,40})/

// One thing wrong with the map, in the map's own terms.
export interface AuditFinding {
  detail: string
  kind: AuditFindingKind
  symbol?: string | undefined
}

// The fail classes. Each names a distinct way the "every stuie-cabi symbol is
// accounted for" claim can be false or unverifiable.
export type AuditFindingKind =
  | 'counterpart-fields-missing'
  | 'counterpart-file-unreadable'
  | 'counterpart-outside-roots'
  | 'empty-map'
  | 'empty-snapshot'
  | 'identifier-absent'
  | 'missing-reason'
  | 'pending-ceiling-exceeded'
  | 'stale-row'
  | 'unmapped-symbol'
  | 'zero-verified-ported'

export interface AuditSummary {
  byKind: ReadonlyMap<AuditFindingKind, number>
  ok: boolean
  total: number
}

// The C++ or JS evidence a `ported` row points at.
export interface CabiCounterpart {
  file: string
  identifier: string
}

export interface CabiMapRow {
  counterpart?: CabiCounterpart | undefined
  reason?: string | undefined
  status: CabiMapRowStatus
  symbol: string
}

export type CabiMapRowStatus = 'out-of-scope' | 'pending' | 'ported'

export interface CabiSymbol {
  file: string
  name: string
  signature: string
  unsafe: boolean
}

export interface CabiSymbolMap {
  pendingCeiling: number
  rows: CabiMapRow[]
}

export interface CabiSymbolSnapshot {
  files: Record<string, string>
  opentuiPin: string
  stuieCommit: string
  symbols: CabiSymbol[]
}

// Byte-order comparator over symbol names, so a snapshot diff stays stable.
export function compareCabiSymbolName(a: CabiSymbol, b: CabiSymbol): number {
  if (a.name === b.name) {
    return 0
  }
  return a.name < b.name ? -1 : 1
}

// Flatten a multi-line Rust signature onto one line for the snapshot.
export function condenseSignature(text: string): string {
  return text.replace(/\s+/g, ' ').trim()
}

// Parse every `pub extern "C" fn` and `pub unsafe extern "C" fn` out of one
// Rust file's text. A multi-line signature is captured whole: the parameter
// list's parens are balanced, then the return type is read up to the body's
// opening brace. Pure over `rustText`, so tests inject fixtures.
export function extractCabiSymbols(
  rustText: string,
  file: string,
): CabiSymbol[] {
  const symbols: CabiSymbol[] = []
  const headerRe = new RegExp(CABI_FN_HEADER_PATTERN, 'gm')
  let header = headerRe.exec(rustText)
  while (header) {
    const closeIndex = findMatchingParenIndex(
      rustText,
      header.index + header[0].length - 1,
    )
    if (closeIndex === -1) {
      break
    }
    let end = closeIndex + 1
    while (
      end < rustText.length &&
      rustText[end] !== '{' &&
      rustText[end] !== ';'
    ) {
      end += 1
    }
    symbols.push({
      file,
      name: header[2] ?? '',
      signature: condenseSignature(rustText.slice(header.index, end)),
      unsafe: Boolean(header[1]),
    })
    headerRe.lastIndex = end
    header = headerRe.exec(rustText)
  }
  return symbols
}

// Index of the `)` closing the `(` at `openIndex`, or -1 when the text runs
// out. Parens nested in a parameter type are balanced, not stopped at.
export function findMatchingParenIndex(
  text: string,
  openIndex: number,
): number {
  let depth = 0
  for (let i = openIndex, { length } = text; i < length; i += 1) {
    const ch = text[i]
    if (ch === '(') {
      depth += 1
    } else if (ch === ')') {
      depth -= 1
      if (depth === 0) {
        return i
      }
    }
  }
  return -1
}

// True when `identifier` appears in `text` on word boundaries.
export function hasWordBoundaryIdentifier(
  text: string,
  identifier: string,
): boolean {
  if (!identifier) {
    return false
  }
  // Word-boundary containment for a counterpart identifier:
  //   `\b`  opening boundary, so `drawBox` never matches inside `drawBoxes`
  //   the escaped identifier, so `::` or `~` in a C++ name stays literal
  //   `\b`  closing boundary
  return new RegExp(`\\b${escapeRegExp(identifier)}\\b`).test(text)
}

// True when a counterpart path sits under one of the allowed roots.
export function isAllowedCounterpartFile(file: string): boolean {
  const normalized = normalizePath(file)
  for (let i = 0, { length } = ALLOWED_COUNTERPART_ROOTS; i < length; i += 1) {
    if (normalized.startsWith(ALLOWED_COUNTERPART_ROOTS[i]!)) {
      return true
    }
  }
  return false
}

// True when `value` is a plain JSON object rather than an array or a scalar.
export function isJsonRecord(value: unknown): value is Record<string, unknown> {
  return typeof value === 'object' && Boolean(value) && !Array.isArray(value)
}

export function isMapRowStatus(value: unknown): value is CabiMapRowStatus {
  return value === 'out-of-scope' || value === 'pending' || value === 'ported'
}

// Narrow a parsed counterpart; an absent field becomes an empty string.
export function narrowCounterpartJson(
  value: unknown,
): CabiCounterpart | undefined {
  if (!isJsonRecord(value)) {
    return undefined
  }
  const file = value['file']
  const identifier = value['identifier']
  return {
    file: typeof file === 'string' ? file : '',
    identifier: typeof identifier === 'string' ? identifier : '',
  }
}

// Narrow parsed snapshot JSON, or undefined when the shape is wrong.
export function narrowSnapshotJson(
  value: unknown,
): CabiSymbolSnapshot | undefined {
  if (!isJsonRecord(value) || !Array.isArray(value['symbols'])) {
    return undefined
  }
  const rawSymbols: unknown[] = value['symbols']
  const symbols: CabiSymbol[] = []
  for (let i = 0, { length } = rawSymbols; i < length; i += 1) {
    const raw = rawSymbols[i]
    if (!isJsonRecord(raw)) {
      return undefined
    }
    const file = raw['file']
    const name = raw['name']
    const signature = raw['signature']
    if (
      typeof file !== 'string' ||
      typeof name !== 'string' ||
      typeof signature !== 'string'
    ) {
      return undefined
    }
    symbols.push({ file, name, signature, unsafe: raw['unsafe'] === true })
  }
  const files: Record<string, string> = {}
  const rawFiles = value['files']
  if (isJsonRecord(rawFiles)) {
    const digests = Object.entries(rawFiles)
    for (let i = 0, { length } = digests; i < length; i += 1) {
      const [key, digest] = digests[i]!
      if (typeof digest === 'string') {
        files[key] = digest
      }
    }
  }
  const opentuiPin = value['opentuiPin']
  const stuieCommit = value['stuieCommit']
  return {
    files,
    opentuiPin: typeof opentuiPin === 'string' ? opentuiPin : '',
    stuieCommit: typeof stuieCommit === 'string' ? stuieCommit : '',
    symbols,
  }
}

// Narrow parsed map JSON, or undefined when the shape is wrong.
export function narrowSymbolMapJson(value: unknown): CabiSymbolMap | undefined {
  if (!isJsonRecord(value) || !Array.isArray(value['rows'])) {
    return undefined
  }
  const pendingCeiling = value['pendingCeiling']
  const rawRows: unknown[] = value['rows']
  if (typeof pendingCeiling !== 'number') {
    return undefined
  }
  const rows: CabiMapRow[] = []
  for (let i = 0, { length } = rawRows; i < length; i += 1) {
    const raw = rawRows[i]
    if (!isJsonRecord(raw)) {
      return undefined
    }
    const status = raw['status']
    const symbol = raw['symbol']
    if (typeof symbol !== 'string' || !isMapRowStatus(status)) {
      return undefined
    }
    const reason = raw['reason']
    rows.push({
      counterpart: narrowCounterpartJson(raw['counterpart']),
      reason: typeof reason === 'string' ? reason : undefined,
      status,
      symbol,
    })
  }
  return { pendingCeiling, rows }
}

// The opentui commit stuie's `.gitmodules` pins, or an empty string.
export function readOpentuiPin(gitmodulesText: string): string {
  return OPENTUI_REF_RE.exec(gitmodulesText)?.[1] ?? ''
}

export function renderFindings(findings: readonly AuditFinding[]): string {
  return findings
    .map(finding => {
      const subject = finding.symbol
        ? `${finding.kind} ${finding.symbol}`
        : finding.kind
      return `    - ${subject} — ${finding.detail}`
    })
    .join('\n')
}

export function summarize(findings: readonly AuditFinding[]): AuditSummary {
  const byKind = new Map<AuditFindingKind, number>()
  for (let i = 0, { length } = findings; i < length; i += 1) {
    const { kind } = findings[i]!
    byKind.set(kind, (byKind.get(kind) ?? 0) + 1)
  }
  return { byKind, ok: findings.length === 0, total: findings.length }
}

// Validate one `ported` row's evidence. Returns the finding that disqualifies
// it, or undefined when the counterpart genuinely carries the identifier.
export function validateCounterpart(
  row: CabiMapRow,
  readCounterpart: (file: string) => string | undefined,
): AuditFinding | undefined {
  const { counterpart } = row
  if (!counterpart?.file || !counterpart.identifier) {
    return {
      detail:
        'status "ported" requires counterpart.file plus counterpart.identifier naming the evidence',
      kind: 'counterpart-fields-missing',
      symbol: row.symbol,
    }
  }
  if (!isAllowedCounterpartFile(counterpart.file)) {
    return {
      detail: `counterpart ${counterpart.file} is outside the allowed roots ${ALLOWED_COUNTERPART_ROOTS.join(' and ')}`,
      kind: 'counterpart-outside-roots',
      symbol: row.symbol,
    }
  }
  const content = readCounterpart(counterpart.file)
  if (content === undefined) {
    return {
      detail: `counterpart ${counterpart.file} is missing or unreadable, so the ported claim cannot be verified`,
      kind: 'counterpart-file-unreadable',
      symbol: row.symbol,
    }
  }
  if (!hasWordBoundaryIdentifier(content, counterpart.identifier)) {
    return {
      detail: `identifier ${counterpart.identifier} does not appear in ${counterpart.file}`,
      kind: 'identifier-absent',
      symbol: row.symbol,
    }
  }
  return undefined
}

// The law itself. Pure over its three inputs — the parsed snapshot, the parsed
// map, and a reader for counterpart files — so every fail class is unit
// testable without a filesystem.
export function validateMap(
  snapshot: CabiSymbolSnapshot,
  map: CabiSymbolMap,
  readCounterpart: (file: string) => string | undefined,
): AuditFinding[] {
  const findings: AuditFinding[] = []
  const { symbols } = snapshot
  const { rows } = map
  if (symbols.length === 0) {
    findings.push({
      detail: `the snapshot lists zero symbols, so a green verdict is vacuous; run \`node ${CHECK_REL_PATH} --update\``,
      kind: 'empty-snapshot',
    })
  }
  if (rows.length === 0) {
    findings.push({
      detail: `the map lists zero rows, so a green verdict is vacuous; curate ${MAP_REL_PATH}`,
      kind: 'empty-map',
    })
  }
  const mapped = new Set<string>()
  for (let i = 0, { length } = rows; i < length; i += 1) {
    mapped.add(rows[i]!.symbol)
  }
  const known = new Set<string>()
  for (let i = 0, { length } = symbols; i < length; i += 1) {
    const symbol = symbols[i]!
    known.add(symbol.name)
    if (!mapped.has(symbol.name)) {
      findings.push({
        detail: `${symbol.file} exports it and no map row covers it; add a row with status ported, out-of-scope, or pending`,
        kind: 'unmapped-symbol',
        symbol: symbol.name,
      })
    }
  }
  let pendingCount = 0
  let portedClaimed = 0
  let portedVerified = 0
  for (let i = 0, { length } = rows; i < length; i += 1) {
    const row = rows[i]!
    if (!known.has(row.symbol)) {
      findings.push({
        detail:
          'no snapshot symbol carries that name; drop the stale row or refresh the snapshot with --update',
        kind: 'stale-row',
        symbol: row.symbol,
      })
    }
    if (row.status === 'ported') {
      portedClaimed += 1
      const finding = validateCounterpart(row, readCounterpart)
      if (finding) {
        findings.push(finding)
      } else {
        portedVerified += 1
      }
      continue
    }
    if (row.status === 'pending') {
      pendingCount += 1
    }
    if (!row.reason?.trim()) {
      findings.push({
        detail: `status "${row.status}" requires a non-empty reason naming the tracker rule or the plan it waits on`,
        kind: 'missing-reason',
        symbol: row.symbol,
      })
    }
  }
  if (pendingCount > map.pendingCeiling) {
    findings.push({
      detail: `${pendingCount} pending rows exceed pendingCeiling ${map.pendingCeiling}; port a symbol rather than raise the ceiling, which only ratchets down`,
      kind: 'pending-ceiling-exceeded',
    })
  }
  if (portedClaimed > 0 && portedVerified === 0) {
    findings.push({
      detail: `${portedClaimed} rows claim ported and none verified, so the ported tier proves nothing`,
      kind: 'zero-verified-ported',
    })
  }
  return findings
}
