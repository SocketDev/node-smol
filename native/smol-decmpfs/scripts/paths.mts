/** @file Shared path + zstd/sysroot resolution for the smol-decmpfs runners. */

import { execFileSync } from 'node:child_process'
import { existsSync } from 'node:fs'
import path from 'node:path'
import process from 'node:process'

// scripts/ -> smol-decmpfs/ -> native/ -> repo root.
export const PKG_ROOT = path.resolve(import.meta.dirname, '..')
export const REPO_ROOT = path.resolve(import.meta.dirname, '../../..')

export const SRC = path.join(PKG_ROOT, 'src')
export const INCLUDE = path.join(PKG_ROOT, 'include')
export const BUILD = path.join(PKG_ROOT, 'build')
export const FUZZ_TARGETS = path.join(PKG_ROOT, 'fuzz', 'fuzz_targets')

export const CORPUS_ROOT = path.join(REPO_ROOT, 'fuzz', 'decmpfs', 'corpus')
export const DICT = path.join(REPO_ROOT, 'fuzz', 'decmpfs', 'decmpfs.dict')
export const GOLDEN = path.join(
  REPO_ROOT,
  'fuzz',
  'decmpfs',
  'verdicts.golden.json',
)

// The two reader source files compiled into every artifact.
export const READER_SOURCES = [
  path.join(SRC, 'sha512.cpp'),
  path.join(SRC, 'smol_decmpfs.cpp'),
]

// C++ harness target -> canonical shared-corpus dir (the Rust target name).
export const CORPUS_DIR_FOR: Readonly<Record<string, string>> = {
  read_hybrid: 'unwrap_if_hybrid',
  decode_pressed: 'decode_pressed_data',
}

/** Homebrew zstd prefix (static lib + headers). Overridable via SMOL_ZSTD_PREFIX. */
export function zstdPrefix(): string | undefined {
  const override = process.env['SMOL_ZSTD_PREFIX']
  if (override && existsSync(path.join(override, 'include', 'zstd.h'))) {
    return override
  }
  for (const candidate of [
    '/opt/homebrew/opt/zstd',
    '/usr/local/opt/zstd',
    '/usr',
    '/usr/local',
  ]) {
    if (existsSync(path.join(candidate, 'include', 'zstd.h'))) {
      return candidate
    }
  }
  return undefined
}

/** Static libzstd path if present, else undefined (fall back to `-lzstd`). */
export function zstdStaticLib(prefix: string): string | undefined {
  for (const name of ['libzstd.a']) {
    const p = path.join(prefix, 'lib', name)
    if (existsSync(p)) {
      return p
    }
  }
  return undefined
}

/** macOS SDK sysroot (full-LLVM clang needs this explicitly). */
export function macSysroot(): string | undefined {
  if (process.platform !== 'darwin') {
    return undefined
  }
  try {
    return execFileSync('xcrun', ['--show-sdk-path'], {
      encoding: 'utf8',
    }).trim()
  } catch {
    return undefined
  }
}
