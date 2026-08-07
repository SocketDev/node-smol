/**
 * @file Shared environment for the binpress integration suites: where the built
 *   binpress binary lives, the mode it was built under, and the Node binary the
 *   suites feed it as input.
 *   Four suites — the ELF PT_NOTE, Mach-O segment and PE section repack tests
 *   plus the target-flag tests — each carried this preamble verbatim. Hoisting
 *   it removes the duplication and keeps every one of them under the file-size
 *   cap.
 */

import path from 'node:path'
import process from 'node:process'
import { fileURLToPath } from 'node:url'

import { getBuildMode } from 'node-smol-packages-build-infra/lib/constants'

const HERE = path.dirname(fileURLToPath(import.meta.url))

/**
 * The binpress package root, one level above `test/`.
 */
export const PACKAGE_DIR = path.join(HERE, '..', '..')

/**
 * `dev` or `prod` — whichever mode the binary under test was built in.
 */
export const BUILD_MODE = getBuildMode()

export const BINPRESS_NAME =
  process.platform === 'win32' ? 'binpress.exe' : 'binpress'

/**
 * The built binpress executable the suites drive.
 */
export const BINPRESS = path.join(
  PACKAGE_DIR,
  'build',
  BUILD_MODE,
  'out',
  'Final',
  BINPRESS_NAME,
)

/**
 * A consistent test input. The running Node binary is used rather than binpress
 * itself, whose size and layout vary with the build.
 */
export const NODE_BINARY = process.execPath
