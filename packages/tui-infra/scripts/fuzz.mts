#!/usr/bin/env node

/**
 * @file `pnpm run test:fuzz:cpp` — tui-infra's C++ fuzz tier (Tier 2 of the
 *   property-and-fuzz-testing skill: libFuzzer under ASan + UBSan). Compiles
 *   one harness from fuzz/fuzz_targets/ together with the sources it
 *   exercises, then runs it against the seed corpus.
 *   Usage:
 *   node packages/tui-infra/scripts/fuzz.mts            # every target
 *   node packages/tui-infra/scripts/fuzz.mts mouse      # one target
 *   FUZZ_TIME_MS=60000 node .../fuzz.mts                # raise the budget
 *   TUI_INFRA_FUZZER_CXX=/path/to/clang++ node .../fuzz.mts
 *   Apple's Xcode clang ships no libFuzzer runtime, so `-fsanitize=fuzzer`
 *   fails at LINK time on a stock macOS box. The compiler is therefore probed
 *   by actually compiling and linking a trivial `LLVMFuzzerTestOneInput`
 *   translation unit, and the first candidate that links wins. When none does
 *   this exits NON-ZERO with the install command — a missing toolchain is a
 *   blocked run, never a fabricated pass.
 */

import { mkdirSync, readdirSync, writeFileSync } from 'node:fs'
import os from 'node:os'
import path from 'node:path'
import process from 'node:process'

import { safeDeleteSync } from '@socketsecurity/lib-stable/fs/safe'
import { getDefaultLogger } from '@socketsecurity/lib-stable/logger/default'
import { spawn } from '@socketsecurity/lib-stable/process/spawn/child'
import { isSpawnError } from '@socketsecurity/lib-stable/process/spawn/errors'

import {
  TUI_FUZZ_BUILD_DIR,
  TUI_FUZZ_CORPUS_DIR,
  TUI_FUZZ_TARGETS_DIR,
  TUI_INCLUDE_SEARCH_DIR,
  TUI_SRC_DIR,
} from '../lib/paths.mts'

const logger = getDefaultLogger()

/**
 * Sources each harness links against, listed per target rather than globbed:
 * a harness pulls in only the translation units it exercises, which keeps the
 * sanitized link small and the coverage signal focused on the code under
 * test.
 */
// oxlint-disable-next-line typescript/no-unsafe-type-assertion -- null-proto idiom: `__proto__: null` keeps the literal prototype-free but changes its inferred type, so the cast is what makes the declared shape hold.
export const FUZZ_TARGET_SOURCES: Record<string, readonly string[]> = {
  __proto__: null,
  mouse: ['mouse.cc'],
  width: ['width.cc', 'width_data.cc'],
} as unknown as Record<string, readonly string[]>

/**
 * Compiler candidates, most-likely-to-work first. Homebrew's LLVM keg carries
 * `libclang_rt.fuzzer_osx.a` and a distro clang gets libFuzzer from
 * compiler-rt. A bare `clang++` is last because on macOS it resolves to
 * Apple's, which compiles the harness fine and only fails at link.
 */
export const FUZZER_CXX_CANDIDATES: readonly string[] = [
  '/opt/homebrew/opt/llvm/bin/clang++',
  '/usr/local/opt/llvm/bin/clang++',
  'clang++',
]

/**
 * Sanitizer + fuzzer flags. `-O1` with frame pointers keeps ASan reports
 * readable while staying fast enough to find something inside a CI budget.
 */
export const SANITIZER_FLAGS: readonly string[] = [
  '-std=c++20',
  '-g',
  '-O1',
  '-fno-omit-frame-pointer',
  '-fsanitize=address,fuzzer,undefined',
  '-fno-sanitize-recover=undefined',
]

/**
 * Run a command and report its exit code instead of throwing. `spawn` rejects
 * on a non-zero exit, but a fuzzer's non-zero exit is the FINDING — the caller
 * needs the code, not an exception.
 */
export async function spawnExitCode(
  cmd: string,
  args: readonly string[],
  options?: { stdio?: 'ignore' | 'inherit' | undefined } | undefined,
): Promise<number> {
  const opts = { __proto__: null, ...options } as { stdio?: string | undefined }
  try {
    // oxlint-disable-next-line typescript/no-unsafe-type-assertion -- null-proto idiom: `__proto__: null` keeps the literal prototype-free but changes its inferred type, so the cast is what makes the declared shape hold.
    await spawn(cmd, args, {
      __proto__: null,
      stdio: opts.stdio ?? 'inherit',
    } as unknown as Parameters<typeof spawn>[2])
    return 0
  } catch (e) {
    if (isSpawnError(e) && typeof e.code === 'number') {
      return e.code
    }
    return 1
  }
}

/**
 * Harness names discovered under fuzz/fuzz_targets/, sorted for a stable run
 * order.
 */
export function readFuzzTargetNames(): string[] {
  return readdirSync(TUI_FUZZ_TARGETS_DIR)
    .filter(name => name.endsWith('.cc'))
    .map(name => name.slice(0, -'.cc'.length))
    .toSorted()
}

/**
 * Compile and link a trivial libFuzzer translation unit with `cxx`. True means
 * the toolchain carries the fuzzer runtime; false means it does not, or the
 * binary is absent.
 */
export async function linksLibFuzzer(cxx: string): Promise<boolean> {
  const probeDir = path.join(os.tmpdir(), `tui-infra-fuzz-probe-${process.pid}`)
  mkdirSync(probeDir, { recursive: true })
  try {
    const source = path.join(probeDir, 'probe.cc')
    const binary = path.join(probeDir, 'probe')
    writeFileSync(
      source,
      '#include <cstddef>\n#include <cstdint>\n' +
        'extern "C" int LLVMFuzzerTestOneInput(const uint8_t*, size_t) { return 0; }\n',
    )
    const code = await spawnExitCode(
      cxx,
      ['-fsanitize=fuzzer', '-o', binary, source],
      { stdio: 'ignore' },
    )
    return code === 0
  } catch {
    return false
  } finally {
    safeDeleteSync(probeDir, { force: true })
  }
}

/**
 * The first candidate compiler that actually links a libFuzzer binary, or
 * undefined when the box has none. `TUI_INFRA_FUZZER_CXX` jumps the queue.
 */
export async function detectFuzzerCompiler(): Promise<string | undefined> {
  const override = process.env['TUI_INFRA_FUZZER_CXX']
  const candidates = override
    ? [override, ...FUZZER_CXX_CANDIDATES]
    : FUZZER_CXX_CANDIDATES
  for (const cxx of candidates) {
    // Sequential on purpose: the first that links wins, and probing the rest
    // after that is wasted compiler spawns.
    const ok = await linksLibFuzzer(cxx)
    if (ok) {
      return cxx
    }
  }
  return undefined
}

/**
 * The compile + link argv for one harness.
 */
export function buildFuzzTargetArgs(config: {
  binary: string
  sources: readonly string[]
  target: string
}): string[] {
  const { binary, sources, target } = config
  return [
    ...SANITIZER_FLAGS,
    `-I${TUI_INCLUDE_SEARCH_DIR}`,
    '-o',
    binary,
    path.join(TUI_FUZZ_TARGETS_DIR, `${target}.cc`),
    ...sources.map(name => path.join(TUI_SRC_DIR, name)),
  ]
}

/**
 * The libFuzzer runtime argv. ASan costs roughly 10x, so the per-input
 * timeout and the memory cap both need headroom over libFuzzer's defaults.
 *
 * LibFuzzer writes new units into the FIRST corpus directory and reads every
 * later one, so the scratch dir leads and the committed seed dir follows.
 * That keeps a run's accretion inside the gitignored build tree instead of
 * growing the tracked corpus on every invocation, and `-artifact_prefix`
 * parks crash reproducers there too rather than in the repo root.
 */
export function runFuzzTargetArgs(config: {
  artifactPrefix: string
  fuzzTimeSeconds: number
  scratchCorpus: string
  seedCorpus: string
}): string[] {
  const { artifactPrefix, fuzzTimeSeconds, scratchCorpus, seedCorpus } = config
  return [
    scratchCorpus,
    seedCorpus,
    `-artifact_prefix=${artifactPrefix}`,
    `-max_total_time=${fuzzTimeSeconds}`,
    '-timeout=30',
    '-rss_limit_mb=4096',
    '-print_final_stats=1',
  ]
}

/**
 * Compile and run one harness. Resolves to the fuzzer's exit code — non-zero
 * means it found a crash, a leak, or a hang.
 */
export async function runFuzzTarget(config: {
  cxx: string
  fuzzTimeSeconds: number
  target: string
}): Promise<number> {
  const { cxx, fuzzTimeSeconds, target } = config
  const sources = FUZZ_TARGET_SOURCES[target]
  if (!sources) {
    logger.error(
      `fuzz: no source list for target \`${target}\`.\n` +
        '  What:  a harness must name the translation units it links.\n' +
        '  Where: FUZZ_TARGET_SOURCES in packages/tui-infra/scripts/fuzz.mts.\n' +
        `  Saw:   a harness at fuzz/fuzz_targets/${target}.cc with no entry; wanted: every harness listed.\n` +
        `  Fix:   add \`${target}: ['<source>.cc']\` to FUZZ_TARGET_SOURCES.`,
    )
    return 1
  }
  const binary = path.join(TUI_FUZZ_BUILD_DIR, target)
  const seedCorpus = path.join(TUI_FUZZ_CORPUS_DIR, target)
  const scratchCorpus = path.join(TUI_FUZZ_BUILD_DIR, `${target}.corpus`)
  const artifactPrefix = path.join(TUI_FUZZ_BUILD_DIR, `${target}.`)
  mkdirSync(TUI_FUZZ_BUILD_DIR, { recursive: true })
  mkdirSync(seedCorpus, { recursive: true })
  mkdirSync(scratchCorpus, { recursive: true })

  logger.log(`fuzz: building ${target} with ${cxx}`)
  const buildCode = await spawnExitCode(
    cxx,
    buildFuzzTargetArgs({ binary, sources, target }),
  )
  if (buildCode !== 0) {
    logger.error(`fuzz: ${target} failed to build.`)
    return buildCode
  }

  logger.log(`fuzz: running ${target} for ${fuzzTimeSeconds}s`)
  return await spawnExitCode(
    binary,
    runFuzzTargetArgs({
      artifactPrefix,
      fuzzTimeSeconds,
      scratchCorpus,
      seedCorpus,
    }),
  )
}

/**
 * The run budget. `FUZZ_TIME_MS` is the same knob the JS lane reads, so one
 * env var raises both; CI sets it above the 15s local default.
 */
export function resolveFuzzTimeSeconds(): number {
  const ms = Number(process.env['FUZZ_TIME_MS']) || 15_000
  return Math.max(1, Math.round(ms / 1000))
}

export async function main(): Promise<void> {
  const requested = process.argv.slice(2).filter(arg => !arg.startsWith('-'))
  const available = readFuzzTargetNames()
  const targets = requested.length > 0 ? requested : available
  const unknown = targets.filter(name => !available.includes(name))
  if (unknown.length > 0) {
    logger.error(
      `fuzz: unknown target(s) ${unknown.join(', ')}.\n` +
        '  What:  a requested harness name matches no file.\n' +
        '  Where: packages/tui-infra/fuzz/fuzz_targets/.\n' +
        `  Saw:   ${unknown.join(', ')}; wanted: one of ${available.join(', ')}.\n` +
        '  Fix:   pass one of those names, or add the harness.',
    )
    process.exitCode = 1
    return
  }

  const cxx = await detectFuzzerCompiler()
  if (!cxx) {
    logger.error(
      'fuzz: smoke-blocked — no libFuzzer-capable clang++ found.\n' +
        '  What:  the C++ fuzz tier needs a clang++ that links `-fsanitize=fuzzer`.\n' +
        `  Where: tried ${FUZZER_CXX_CANDIDATES.join(', ')} (override with TUI_INFRA_FUZZER_CXX).\n` +
        "  Saw:   every candidate failed to link a trivial LLVMFuzzerTestOneInput translation unit; wanted: one that links it. Apple's Xcode clang ships no libFuzzer runtime.\n" +
        '  Fix:   brew install llvm  (macOS)  |  apt install clang lld  (Linux)',
    )
    process.exitCode = 1
    return
  }

  const fuzzTimeSeconds = resolveFuzzTimeSeconds()
  for (const target of targets) {
    // Sequential on purpose: each target saturates the box under ASan, and a
    // finding should stop the run rather than race the next build.
    const code = await runFuzzTarget({ cxx, fuzzTimeSeconds, target })
    if (code !== 0) {
      logger.error(`fuzz: ${target} exited ${code}.`)
      process.exitCode = code
      return
    }
  }
  logger.success(`fuzz: ${targets.length} C++ target(s) clean.`)
}

void main()
