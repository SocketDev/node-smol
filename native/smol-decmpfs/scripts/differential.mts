#!/usr/bin/env node
/**
 * @file Lock-step differential check: the C++ inverse reader vs the canonical
 *   Rust reference (`decmpfs::addon`), over the SHARED corpus at
 *   fuzz/decmpfs/corpus/. This IS the cross-lane contract — for every input the
 *   two lanes must agree on accept/reject AND on the decoded output.
 *
 *   Default mode compares the C++ reader against the committed golden verdict
 *   table (fuzz/decmpfs/verdicts.golden.json), which was minted from the Rust
 *   reference (per docs/agents.md/fleet/golden-fixtures.md — a golden is
 *   authority-verified reference output). `--update-golden` rebuilds the table
 *   by running the Rust reference itself (a throwaway cargo crate that
 *   path-depends on upstream/decmpfs/crates/decmpfs with the `addon` feature),
 *   proving the golden still reflects Rust.
 *
 *   Usage:
 *     node scripts/differential.mts                 # C++ vs golden
 *     node scripts/differential.mts --update-golden # regenerate from Rust
 */

import { spawnSync } from 'node:child_process'
import {
  mkdirSync,
  mkdtempSync,
  readFileSync,
  readdirSync,
  rmSync,
  writeFileSync,
} from 'node:fs'
import os from 'node:os'
import path from 'node:path'
import process from 'node:process'

import {
  BUILD,
  CORPUS_DIR_FOR,
  CORPUS_ROOT,
  GOLDEN,
  INCLUDE,
  PKG_ROOT,
  READER_SOURCES,
  REPO_ROOT,
  SRC,
  zstdPrefix,
  zstdStaticLib,
} from './paths.mts'

const TARGETS = ['read_hybrid', 'decode_pressed'] as const
type Target = (typeof TARGETS)[number]

/** Build the C++ decode CLI with Apple/xcrun clang (SDK sysroot resolved). */
function buildCppCli(): string {
  const prefix = zstdPrefix()
  if (!prefix) {
    throw new Error('libzstd not found (brew install zstd).')
  }
  mkdirSync(BUILD, { recursive: true })
  const out = path.join(BUILD, 'decode_cli')
  const staticLib = zstdStaticLib(prefix)
  const args = [
    'clang++',
    '-std=c++17',
    '-O1',
    '-g',
    '-Wall',
    '-Wextra',
    `-I${INCLUDE}`,
    `-I${SRC}`,
    `-I${path.join(prefix, 'include')}`,
    ...READER_SOURCES,
    path.join(PKG_ROOT, 'tool', 'decode_cli.cpp'),
    staticLib ?? '-lzstd',
  ]
  if (!staticLib) {
    args.push(`-L${path.join(prefix, 'lib')}`)
  }
  args.push('-o', out)
  // `xcrun clang++` resolves the macOS SDK sysroot; on Linux xcrun is absent so
  // fall back to invoking the compiler directly.
  const useXcrun = process.platform === 'darwin'
  const cmd = useXcrun ? 'xcrun' : args[0]!
  const cmdArgs = useXcrun ? args : args.slice(1)
  const r = spawnSync(cmd, cmdArgs, { stdio: 'inherit' })
  if (r.status !== 0) {
    throw new Error('C++ decode_cli build failed')
  }
  return out
}

function corpusFiles(target: Target): string[] {
  const dir = path.join(CORPUS_ROOT, CORPUS_DIR_FOR[target]!)
  return readdirSync(dir)
    .sort((a, b) => a.localeCompare(b))
    .map(name => path.join(dir, name))
}

/** Run a verdict producer (CLI path) over the whole corpus -> { key: verdict }. */
function collectVerdicts(bin: string): Record<string, string> {
  const out: Record<string, string> = Object.create(null)
  for (const target of TARGETS) {
    for (const file of corpusFiles(target)) {
      const r = spawnSync(bin, [target, file], { encoding: 'utf8' })
      if (r.status !== 0) {
        throw new Error(`${bin} ${target} ${file} exited ${r.status}`)
      }
      const verdict = (r.stdout ?? '').trim().replace(/\t/g, ' ')
      out[`${target}/${path.basename(file)}`] = verdict
    }
  }
  return out
}

/** Build a throwaway Rust oracle in a temp dir; never touches upstream/decmpfs. */
function buildRustOracle(): string {
  const crate = path.join(REPO_ROOT, 'upstream', 'decmpfs', 'crates', 'decmpfs')
  const tmp = mkdtempSync(path.join(os.tmpdir(), 'smol-decmpfs-oracle-'))
  mkdirSync(path.join(tmp, 'src'), { recursive: true })
  writeFileSync(
    path.join(tmp, 'Cargo.toml'),
    `[package]\nname = "smol-decmpfs-oracle"\nversion = "0.0.0"\nedition = "2021"\npublish = false\n\n[dependencies]\ndecmpfs = { path = ${JSON.stringify(crate)}, features = ["addon"] }\nsha2 = "0.10"\n\n[workspace]\n`,
  )
  writeFileSync(
    path.join(tmp, 'src', 'main.rs'),
    [
      'use sha2::{Digest, Sha512};',
      'use std::io::Read;',
      'fn main() {',
      '  let a: Vec<String> = std::env::args().collect();',
      '  let mut data = Vec::new();',
      '  if let Ok(mut f) = std::fs::File::open(&a[2]) { let _ = f.read_to_end(&mut data); }',
      '  let r = match a[1].as_str() {',
      '    "read_hybrid" => decmpfs::addon::unwrap_if_hybrid(&data),',
      '    "decode_pressed" => decmpfs::addon::decode_pressed_data(&data),',
      '    _ => std::process::exit(2),',
      '  };',
      '  match r {',
      '    None => println!("reject"),',
      '    Some(o) => {',
      '      let mut h = Sha512::new(); h.update(&o);',
      '      let hex: String = h.finalize().iter().map(|b| format!("{b:02x}")).collect();',
      '      println!("accept\\t{}\\t{}", o.len(), hex);',
      '    }',
      '  }',
      '}',
    ].join('\n'),
  )
  const build = spawnSync('cargo', ['build', '--release', '--offline'], {
    cwd: tmp,
    stdio: 'inherit',
  })
  if (build.status !== 0) {
    const online = spawnSync('cargo', ['build', '--release'], {
      cwd: tmp,
      stdio: 'inherit',
    })
    if (online.status !== 0) {
      throw new Error('Rust oracle build failed (cargo unavailable/offline).')
    }
  }
  return path.join(tmp, 'target', 'release', 'smol-decmpfs-oracle')
}

function compare(
  cpp: Record<string, string>,
  golden: Record<string, string>,
): string[] {
  const divergences: string[] = []
  const keys = new Set([...Object.keys(cpp), ...Object.keys(golden)])
  for (const key of [...keys].sort((a, b) => a.localeCompare(b))) {
    const a = cpp[key] ?? '(missing)'
    const b = golden[key] ?? '(missing)'
    const ok = a === b
    console.log(`${ok ? 'ok  ' : 'DIFF'}  ${key}  cpp=[${a}] rust=[${b}]`)
    if (!ok) {
      divergences.push(key)
    }
  }
  return divergences
}

function main(): void {
  const updateGolden = process.argv.includes('--update-golden')
  let cli: string
  try {
    cli = buildCppCli()
  } catch (e) {
    console.error(String(e))
    process.exitCode = 1
    return
  }
  const cppVerdicts = collectVerdicts(cli)

  if (updateGolden) {
    let oracle: string
    try {
      oracle = buildRustOracle()
    } catch (e) {
      console.error(String(e))
      process.exitCode = 1
      return
    }
    const rust = collectVerdicts(oracle)
    const divergences = compare(cppVerdicts, rust)
    if (divergences.length) {
      console.error(
        `DIVERGENCE: ${divergences.length} input(s) disagree with Rust — refusing to write golden.`,
      )
      process.exitCode = 1
      return
    }
    const body: Record<string, string> = {
      _comment:
        'Golden verdicts minted from the canonical Rust reference (decmpfs::addon). ' +
        'accept = Some(bytes) with sha512 of decoded output; reject = None. The C++ ' +
        'inverse reader MUST reproduce every entry. Regenerate with ' +
        'scripts/differential.mts --update-golden.',
      ...rust,
    }
    writeFileSync(GOLDEN, `${JSON.stringify(body, null, 2)}\n`)
    console.log(`Wrote golden: ${GOLDEN}`)
    return
  }

  const raw = JSON.parse(readFileSync(GOLDEN, 'utf8')) as Record<string, string>
  const golden: Record<string, string> = {}
  for (const [k, v] of Object.entries(raw)) {
    if (k !== '_comment') {
      golden[k] = v
    }
  }
  const divergences = compare(cppVerdicts, golden)
  if (divergences.length) {
    console.error(
      `\nDIVERGENCE: ${divergences.length} input(s) — C++ reader disagrees with the Rust golden.`,
    )
    process.exitCode = 1
    return
  }
  console.log(
    `\nlock-step OK: ${Object.keys(golden).length} corpus inputs agree (accept/reject + output).`,
  )
}

main()
