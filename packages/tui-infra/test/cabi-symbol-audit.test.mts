/**
 * The stuie-cabi symbol audit's law, exercised through its pure core: the
 * extractor must see every `extern "C"` declaration form, and the validator
 * must report each fail class that would let an unmapped or unverifiable
 * symbol claim pass. Every counterpart read is injected, so no test touches
 * the real filesystem.
 */

import { describe, expect, it } from 'vitest'

import {
  extractCabiSymbols,
  hasWordBoundaryIdentifier,
  isAllowedCounterpartFile,
  narrowSymbolMapJson,
  readOpentuiPin,
  renderFindings,
  summarize,
  validateMap,
} from '../../../scripts/repo/check/smol-tui-cabi-symbols-are-mapped.mts'
import type {
  AuditFinding,
  AuditFindingKind,
  CabiMapRow,
  CabiSymbol,
  CabiSymbolMap,
  CabiSymbolSnapshot,
} from '../../../scripts/repo/check/smol-tui-cabi-symbols-are-mapped.mts'

const CABI_FILE = 'crates/stuie-cabi/src/cabi.rs'

const PORTED_FILE = 'packages/tui-infra/src/renderer.cc'

function findingKinds(findings: readonly AuditFinding[]): AuditFindingKind[] {
  return findings.map(finding => finding.kind)
}

function makeMap(rows: CabiMapRow[], pendingCeiling: number): CabiSymbolMap {
  return { pendingCeiling, rows }
}

function makeSnapshot(symbols: CabiSymbol[]): CabiSymbolSnapshot {
  return {
    files: { [CABI_FILE]: 'a'.repeat(64) },
    opentuiPin: '0c8c4f7cff2927e3df63a9757a45eff9a343611c',
    stuieCommit: 'ae927afb9e8176dc4c7b642a1d8e638285df448a',
    symbols,
  }
}

function makeSymbol(name: string): CabiSymbol {
  return {
    file: CABI_FILE,
    name,
    signature: `pub extern "C" fn ${name}(handle: u32)`,
    unsafe: false,
  }
}

function portedRow(symbol: string, identifier: string): CabiMapRow {
  return {
    counterpart: { file: PORTED_FILE, identifier },
    status: 'ported',
    symbol,
  }
}

describe('extractCabiSymbols', () => {
  it('captures a single-line signature including its return type', () => {
    const rustText = [
      '#[no_mangle]',
      'pub extern "C" fn bufferGetWidth(handle: u32) -> u32 {',
      '    width_of(handle)',
      '}',
    ].join('\n')
    const symbols = extractCabiSymbols(rustText, CABI_FILE)
    expect(symbols).toEqual([
      {
        file: CABI_FILE,
        name: 'bufferGetWidth',
        signature: 'pub extern "C" fn bufferGetWidth(handle: u32) -> u32',
        unsafe: false,
      },
    ])
  })

  it('captures a multi-line signature through the closing paren', () => {
    const rustText = [
      '#[no_mangle]',
      'pub extern "C" fn bufferDrawText(',
      '    handle: u32,',
      '    text_ptr: *const u8,',
      '    text_len: u32,',
      ') -> bool {',
      '    true',
      '}',
    ].join('\n')
    const symbols = extractCabiSymbols(rustText, CABI_FILE)
    expect(symbols).toHaveLength(1)
    expect(symbols[0]?.name).toBe('bufferDrawText')
    expect(symbols[0]?.signature).toBe(
      'pub extern "C" fn bufferDrawText( handle: u32, text_ptr: *const u8, text_len: u32, ) -> bool',
    )
  })

  it('flags a pub unsafe extern "C" fn as unsafe', () => {
    const rustText = [
      'pub unsafe extern "C" fn freeUnicode(ptr: *mut EncodedChar, len: u32) {',
      '    drop(ptr);',
      '}',
    ].join('\n')
    const symbols = extractCabiSymbols(rustText, CABI_FILE)
    expect(symbols[0]?.unsafe).toBe(true)
    expect(symbols[0]?.name).toBe('freeUnicode')
  })

  it('attributes symbols to the file they were extracted from', () => {
    const audioFile = 'crates/stuie-cabi/src/audio_decoder.rs'
    const fromCabi = extractCabiSymbols(
      'pub extern "C" fn bufferClear(handle: u32) {}',
      CABI_FILE,
    )
    const fromAudio = extractCabiSymbols(
      'pub extern "C" fn audioDecoderClose(handle: u32) {}',
      audioFile,
    )
    const symbols = [...fromCabi, ...fromAudio]
    expect(symbols.map(symbol => symbol.file)).toEqual([CABI_FILE, audioFile])
    expect(symbols.map(symbol => symbol.name)).toEqual([
      'bufferClear',
      'audioDecoderClose',
    ])
  })

  it('ignores non-extern fns and doc lines that quote the extern form', () => {
    const rustText = [
      'fn helper_one(handle: u32) {}',
      'pub fn helper_two(handle: u32) {}',
      'pub extern "Rust" fn helper_three(handle: u32) {}',
      '/// pub extern "C" fn documentedOnly(handle: u32)',
      'pub extern "C" fn realExport(handle: u32) {}',
    ].join('\n')
    const symbols = extractCabiSymbols(rustText, CABI_FILE)
    expect(symbols.map(symbol => symbol.name)).toEqual(['realExport'])
  })

  it('returns nothing for a file with no C ABI exports', () => {
    expect(extractCabiSymbols('pub struct Buffer {}', CABI_FILE)).toEqual([])
  })
})

describe('validateMap', () => {
  it('reports a snapshot symbol that no map row covers', () => {
    const snapshot = makeSnapshot([
      makeSymbol('bufferClear'),
      makeSymbol('bufferDrawText'),
    ])
    const map = makeMap([portedRow('bufferClear', 'Clear')], 0)
    const findings = validateMap(snapshot, map, () => 'void Clear() {}')
    expect(findingKinds(findings)).toEqual(['unmapped-symbol'])
    expect(findings[0]?.symbol).toBe('bufferDrawText')
  })

  it('reports a map row whose symbol is absent from the snapshot', () => {
    const snapshot = makeSnapshot([makeSymbol('bufferClear')])
    const map = makeMap(
      [
        portedRow('bufferClear', 'Clear'),
        portedRow('bufferRemovedLastYear', 'Clear'),
      ],
      0,
    )
    const findings = validateMap(snapshot, map, () => 'void Clear() {}')
    expect(findingKinds(findings)).toEqual(['stale-row'])
    expect(findings[0]?.symbol).toBe('bufferRemovedLastYear')
  })

  it('reports a ported row missing its counterpart fields', () => {
    const snapshot = makeSnapshot([makeSymbol('bufferClear')])
    const map = makeMap([{ status: 'ported', symbol: 'bufferClear' }], 0)
    const findings = validateMap(snapshot, map, () => 'void Clear() {}')
    expect(findingKinds(findings)).toEqual([
      'counterpart-fields-missing',
      'zero-verified-ported',
    ])
  })

  it('reports a counterpart identifier that is absent from the file', () => {
    const snapshot = makeSnapshot([
      makeSymbol('bufferClear'),
      makeSymbol('bufferDrawText'),
    ])
    const map = makeMap(
      [
        portedRow('bufferClear', 'Clear'),
        portedRow('bufferDrawText', 'DrawTextMissing'),
      ],
      0,
    )
    const findings = validateMap(snapshot, map, () => 'void Clear() {}')
    expect(findingKinds(findings)).toEqual(['identifier-absent'])
    expect(findings[0]?.symbol).toBe('bufferDrawText')
  })

  it('reports a counterpart file outside the allowed roots', () => {
    const snapshot = makeSnapshot([makeSymbol('bufferClear')])
    const map = makeMap(
      [
        {
          counterpart: {
            file: 'crates/stuie-cabi/src/cabi.rs',
            identifier: 'bufferClear',
          },
          status: 'ported',
          symbol: 'bufferClear',
        },
      ],
      0,
    )
    const findings = validateMap(snapshot, map, () => 'fn bufferClear() {}')
    expect(findingKinds(findings)).toEqual([
      'counterpart-outside-roots',
      'zero-verified-ported',
    ])
  })

  it('reports a counterpart file the reader cannot read', () => {
    const snapshot = makeSnapshot([
      makeSymbol('bufferClear'),
      makeSymbol('bufferDrawText'),
    ])
    const map = makeMap(
      [
        portedRow('bufferClear', 'Clear'),
        {
          counterpart: {
            file: 'packages/tui-infra/src/gone.cc',
            identifier: 'Gone',
          },
          status: 'ported',
          symbol: 'bufferDrawText',
        },
      ],
      0,
    )
    const findings = validateMap(snapshot, map, file =>
      file === PORTED_FILE ? 'void Clear() {}' : undefined,
    )
    expect(findingKinds(findings)).toEqual(['counterpart-file-unreadable'])
    expect(findings[0]?.symbol).toBe('bufferDrawText')
  })

  it('reports an out-of-scope or pending row with an empty reason', () => {
    const snapshot = makeSnapshot([
      makeSymbol('audioDecoderOpen'),
      makeSymbol('editBufferInsert'),
    ])
    const map = makeMap(
      [
        { reason: '   ', status: 'out-of-scope', symbol: 'audioDecoderOpen' },
        { status: 'pending', symbol: 'editBufferInsert' },
      ],
      2,
    )
    const findings = validateMap(snapshot, map, () => undefined)
    expect(findingKinds(findings)).toEqual(['missing-reason', 'missing-reason'])
    expect(findings.map(finding => finding.symbol)).toEqual([
      'audioDecoderOpen',
      'editBufferInsert',
    ])
  })

  it('reports a pending count above the ceiling', () => {
    const snapshot = makeSnapshot([
      makeSymbol('editBufferInsert'),
      makeSymbol('textViewScroll'),
    ])
    const map = makeMap(
      [
        {
          reason: 'waits on the editBuffer tier',
          status: 'pending',
          symbol: 'editBufferInsert',
        },
        {
          reason: 'waits on the textView tier',
          status: 'pending',
          symbol: 'textViewScroll',
        },
      ],
      1,
    )
    const findings = validateMap(snapshot, map, () => undefined)
    expect(findingKinds(findings)).toEqual(['pending-ceiling-exceeded'])
    expect(findings[0]?.detail).toContain('exceed pendingCeiling 1')
  })

  it('reports an empty snapshot as a vacuous green', () => {
    const map = makeMap([portedRow('bufferClear', 'Clear')], 0)
    const findings = validateMap(makeSnapshot([]), map, () => 'void Clear() {}')
    expect(findingKinds(findings)).toContain('empty-snapshot')
  })

  it('reports an empty map as a vacuous green', () => {
    const snapshot = makeSnapshot([makeSymbol('bufferClear')])
    const findings = validateMap(snapshot, makeMap([], 0), () => undefined)
    expect(findingKinds(findings)).toEqual(['empty-map', 'unmapped-symbol'])
  })

  it('reports a map that claims ported rows but verifies none', () => {
    const snapshot = makeSnapshot([
      makeSymbol('bufferClear'),
      makeSymbol('bufferDrawText'),
    ])
    const map = makeMap(
      [
        portedRow('bufferClear', 'MissingOne'),
        portedRow('bufferDrawText', 'MissingTwo'),
      ],
      0,
    )
    const findings = validateMap(snapshot, map, () => 'void Unrelated() {}')
    expect(findingKinds(findings)).toEqual([
      'identifier-absent',
      'identifier-absent',
      'zero-verified-ported',
    ])
  })

  it('passes a fully mapped, fully evidenced fixture', () => {
    const snapshot = makeSnapshot([
      makeSymbol('audioDecoderOpen'),
      makeSymbol('bufferClear'),
      makeSymbol('editBufferInsert'),
    ])
    const map = makeMap(
      [
        {
          reason: 'audio is out of scope per the lockstep tracker audio rule',
          status: 'out-of-scope',
          symbol: 'audioDecoderOpen',
        },
        portedRow('bufferClear', 'Clear'),
        {
          reason:
            'waits on the editBuffer tier in docs/ports/smol-tui-lockstep.md',
          status: 'pending',
          symbol: 'editBufferInsert',
        },
      ],
      1,
    )
    const findings = validateMap(snapshot, map, () => 'void Clear() {}')
    expect(findings).toEqual([])
    expect(summarize(findings).ok).toBe(true)
  })
})

describe('audit reporting helpers', () => {
  it('summarizes findings by kind', () => {
    const findings: AuditFinding[] = [
      { detail: 'one', kind: 'unmapped-symbol', symbol: 'a' },
      { detail: 'two', kind: 'unmapped-symbol', symbol: 'b' },
      { detail: 'three', kind: 'empty-map' },
    ]
    const summary = summarize(findings)
    expect(summary.total).toBe(3)
    expect(summary.ok).toBe(false)
    expect(summary.byKind.get('unmapped-symbol')).toBe(2)
    expect(summary.byKind.get('empty-map')).toBe(1)
  })

  it('renders one line per finding with its symbol and detail', () => {
    const rendered = renderFindings([
      {
        detail: 'no row covers it',
        kind: 'unmapped-symbol',
        symbol: 'bufferX',
      },
      { detail: 'zero rows', kind: 'empty-map' },
    ])
    const lines = rendered.split('\n')
    expect(lines).toHaveLength(2)
    expect(lines[0]).toContain('unmapped-symbol bufferX')
    expect(lines[0]).toContain('no row covers it')
    expect(lines[1]).toContain('empty-map')
  })
})

describe('audit predicates', () => {
  it('matches a counterpart identifier only on word boundaries', () => {
    expect(hasWordBoundaryIdentifier('void DrawBox() {}', 'DrawBox')).toBe(true)
    expect(hasWordBoundaryIdentifier('void DrawBoxes() {}', 'DrawBox')).toBe(
      false,
    )
    expect(hasWordBoundaryIdentifier('void DrawBox() {}', '')).toBe(false)
  })

  it('accepts counterpart files only under the allowed roots', () => {
    expect(isAllowedCounterpartFile('packages/tui-infra/src/renderer.cc')).toBe(
      true,
    )
    expect(
      isAllowedCounterpartFile(
        'packages/node-smol-builder/additions/source-patched/lib/smol-tui.js',
      ),
    ).toBe(true)
    expect(isAllowedCounterpartFile('crates/stuie-cabi/src/cabi.rs')).toBe(
      false,
    )
  })

  it('reads the opentui pin out of a gitmodules block', () => {
    const gitmodules = [
      '[submodule "packages/core/upstream/opentui"]',
      '\tref = 0c8c4f7cff2927e3df63a9757a45eff9a343611c',
      '\tpath = packages/core/upstream/opentui',
      '[submodule "crates/yoga-open-tui/upstream/yoga"]',
      '\tref = 042f5013152eb81c1552dec945b88f7b95ca350f',
    ].join('\n')
    expect(readOpentuiPin(gitmodules)).toBe(
      '0c8c4f7cff2927e3df63a9757a45eff9a343611c',
    )
    expect(readOpentuiPin('[submodule "other"]')).toBe('')
  })

  it('rejects map JSON whose row status is not a known tier', () => {
    expect(
      narrowSymbolMapJson({
        pendingCeiling: 0,
        rows: [{ status: 'portd', symbol: 'bufferClear' }],
      }),
    ).toBeUndefined()
    expect(
      narrowSymbolMapJson({
        pendingCeiling: 1,
        rows: [{ reason: 'waits', status: 'pending', symbol: 'bufferClear' }],
      })?.rows[0]?.status,
    ).toBe('pending')
  })
})
