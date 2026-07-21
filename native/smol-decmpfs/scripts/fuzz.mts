#!/usr/bin/env node
/**
 * @file libFuzzer + ASan/UBSan runner for the C++ smol-decmpfs inverse reader.
 *   Lock-step port of the Rust lane's fuzz targets (decmpfs/fuzz/run.sh); the
 *   two targets `read_hybrid` / `decode_pressed` mirror the Rust
 *   `unwrap_if_hybrid` / `decode_pressed_data` and seed from the SHARED corpus
 *   at fuzz/decmpfs/corpus/.
 *
 *   Apple's Xcode clang has NO libFuzzer runtime, so this probes for a
 *   full-LLVM clang++ (`brew install llvm`) by compiling + linking a trivial
 *   LLVMFuzzerTestOneInput TU, and uses the first that links. When none is
 *   found it prints `smoke-blocked: libFuzzer-capable clang++ absent` with the
 *   install command and exits non-zero — never a fabricated pass.
 *
 *   Usage:
 *     node scripts/fuzz.mts list
 *     node scripts/fuzz.mts build <target>
 *     node scripts/fuzz.mts run   <target> [--time <sec>]
 */

import { spawnSync } from 'node:child_process'
import { existsSync, mkdirSync, mkdtempSync, rmSync, writeFileSync } from 'node:fs'
import os from 'node:os'
import path from 'node:path'
import process from 'node:process'

import {
  BUILD,
  CORPUS_DIR_FOR,
  CORPUS_ROOT,
  DICT,
  FUZZ_TARGETS,
  INCLUDE,
  READER_SOURCES,
  SRC,
  macSysroot,
  zstdPrefix,
  zstdStaticLib,
} from './paths.mts'

const TARGETS = ['read_hybrid', 'decode_pressed'] as const
type Target = (typeof TARGETS)[number]

const FUZZER_BUILD = path.join(BUILD, 'fuzzer')

const INSTALL_HINT =
  'smoke-blocked: libFuzzer-capable clang++ absent. Install a full LLVM ' +
  '(NOT Apple/Xcode clang, which carries no libFuzzer runtime):\n' +
  '  macOS: brew install llvm\n' +
  '  Linux: apt install clang lld\n' +
  'Then re-run. Set SMOL_FUZZER_CXX=/path/to/clang++ to point at a nonstandard install.'

function isTarget(v: string | undefined): v is Target {
  return !!v && (TARGETS as readonly string[]).includes(v)
}

function candidateCompilers(): string[] {
  const list: string[] = []
  if (process.env['SMOL_FUZZER_CXX']) {
    list.push(process.env['SMOL_FUZZER_CXX']!)
  }
  list.push(
    '/opt/homebrew/opt/llvm/bin/clang++',
    '/usr/local/opt/llvm/bin/clang++',
  )
  // Versioned Homebrew kegs, newest first.
  for (const v of [20, 19, 18, 17, 16]) {
    list.push(`/opt/homebrew/opt/llvm@${v}/bin/clang++`)
    list.push(`/usr/local/opt/llvm@${v}/bin/clang++`)
    list.push(`clang++-${v}`)
  }
  list.push('clang++')
  return list
}

/** Compile + link a trivial libFuzzer TU; return the first compiler that works. */
function detectFuzzerCompiler(): string | undefined {
  const sysroot = macSysroot()
  const tmp = mkdtempSync(path.join(os.tmpdir(), 'smol-fuzz-probe-'))
  try {
    const srcFile = path.join(tmp, 'probe.cc')
    writeFileSync(
      srcFile,
      '#include <cstdint>\n#include <cstddef>\n' +
        'extern "C" int LLVMFuzzerTestOneInput(const uint8_t*, size_t){return 0;}\n',
    )
    const outFile = path.join(tmp, 'probe')
    for (const cxx of candidateCompilers()) {
      const args = ['-fsanitize=fuzzer', '-o', outFile, srcFile]
      if (sysroot) {
        args.unshift('-isysroot', sysroot)
      }
      const r = spawnSync(cxx, args, { stdio: 'ignore' })
      if (r.status === 0 && existsSync(outFile)) {
        return cxx
      }
      rmSync(outFile, { force: true })
    }
    return undefined
  } finally {
    rmSync(tmp, { force: true, recursive: true })
  }
}

function binaryPath(target: Target): string {
  return path.join(FUZZER_BUILD, `fuzz_${target}`)
}

function build(target: Target): boolean {
  const cxx = detectFuzzerCompiler()
  if (!cxx) {
    console.error(INSTALL_HINT)
    return false
  }
  const prefix = zstdPrefix()
  if (!prefix) {
    console.error('smoke-blocked: libzstd not found (brew install zstd).')
    return false
  }
  mkdirSync(FUZZER_BUILD, { recursive: true })
  const sysroot = macSysroot()
  const staticLib = zstdStaticLib(prefix)
  const args: string[] = [
    '-std=c++17',
    '-g',
    '-O1',
    '-fsanitize=address,fuzzer,undefined',
    '-fno-sanitize-recover=undefined',
    `-I${INCLUDE}`,
    `-I${SRC}`,
    `-I${path.join(prefix, 'include')}`,
  ]
  if (sysroot) {
    args.push('-isysroot', sysroot)
  }
  args.push(
    ...READER_SOURCES,
    path.join(FUZZ_TARGETS, `${target}.cc`),
    staticLib ?? '-lzstd',
  )
  if (!staticLib) {
    args.push(`-L${path.join(prefix, 'lib')}`)
  }
  args.push('-o', binaryPath(target))
  console.log(`Building fuzz_${target} with ${cxx}`)
  const r = spawnSync(cxx, args, { stdio: 'inherit' })
  if (r.status !== 0) {
    console.error(`build failed for ${target}`)
    return false
  }
  return true
}

function run(target: Target, timeSec: number): number {
  if (!build(target)) {
    return 1
  }
  // The SHARED corpus is a curated, committed seed set — read-only. libFuzzer
  // writes coverage-increasing discoveries into its FIRST positional dir, so
  // point that at a gitignored working dir and pass the shared corpus as a
  // read-only seed. Never let a run mutate the committed corpus.
  const seedDir = path.join(CORPUS_ROOT, CORPUS_DIR_FOR[target]!)
  const workingDir = path.join(FUZZER_BUILD, `corpus-${target}`)
  mkdirSync(workingDir, { recursive: true })
  const artifacts = path.join(FUZZER_BUILD, `artifacts-${target}`)
  mkdirSync(artifacts, { recursive: true })
  const flags = [
    workingDir,
    seedDir,
    `-max_total_time=${timeSec}`,
    '-timeout=10',
    '-rss_limit_mb=2048',
    '-max_len=65536',
    '-print_final_stats=1',
    `-artifact_prefix=${artifacts}${path.sep}`,
  ]
  if (existsSync(DICT)) {
    flags.push(`-dict=${DICT}`)
  }
  console.log(`Fuzzing ${target} for ${timeSec}s (seed: ${seedDir})`)
  const r = spawnSync(binaryPath(target), flags, {
    stdio: 'inherit',
    env: { ...process.env, ASAN_OPTIONS: 'detect_leaks=0' },
  })
  return r.status ?? 1
}

function main(): void {
  const argv = process.argv.slice(2)
  const cmd = argv[0] ?? 'run'
  if (cmd === 'list') {
    console.log(`Fuzz targets: ${TARGETS.join(', ')}`)
    return
  }
  if (cmd === 'build') {
    const t = argv[1]
    if (!isTarget(t)) {
      console.error(`unknown target: ${t ?? '(none)'}`)
      process.exitCode = 2
      return
    }
    process.exitCode = build(t) ? 0 : 1
    return
  }
  // run
  const rest = cmd === 'run' ? argv.slice(1) : argv
  const t = rest[0]
  if (!isTarget(t)) {
    console.error(`unknown target: ${t ?? '(none)'}. Valid: ${TARGETS.join(', ')}`)
    process.exitCode = 2
    return
  }
  const ti = rest.indexOf('--time')
  const timeSec = ti >= 0 ? Number(rest[ti + 1]) : 60
  process.exitCode = run(t, timeSec)
}

main()
