/**
 * @file Fixtures for the cross-tool repack suite: resolving a built binsuite
 *   binary's path, and running a command with its output captured.
 *   Both were exported from the spec itself, which made a test file the home of
 *   reusable fixtures and pushed it past the file-size cap.
 */

import path from 'node:path'
import process from 'node:process'

import { getBuildMode } from 'build-infra/lib/constants'

import { spawn } from '@socketsecurity/lib-stable/process/spawn/child'

import type { SpawnOptions } from '@socketsecurity/lib-stable/process/spawn/types'

// Package binaries
const BUILD_MODE = getBuildMode()

export async function getBinaryPath(packageName: string, binaryName: string) {
  const ext = process.platform === 'win32' ? '.exe' : ''
  const platformArch = await getCurrentPlatformArch()
  // Try platform-arch path first (includes -musl suffix on Alpine), then legacy path without it.
  const withPlatform = path.join(
    REPO_ROOT,
    'packages',
    packageName,
    'build',
    BUILD_MODE,
    platformArch,
    'out',
    'Final',
    binaryName + ext,
  )
  if (existsSync(withPlatform)) {
    return withPlatform
  }
  return path.join(
    REPO_ROOT,
    'packages',
    packageName,
    'build',
    BUILD_MODE,
    'out',
    'Final',
    binaryName + ext,
  )
}

/**
 * Execute command.
 */
// Test helpers are ordered by the repack flow they exercise; alphabetizing
// would scatter them across the file.
// oxlint-disable-next-line socket/sort-source-methods -- intentional ordering
export async function execCommand(
  command: string,
  args: string[] = [],
  options: SpawnOptions = {},
) {
  const result = await spawn(command, args, {
    ...options,
    stdio: 'pipe',
  })
  return {
    code: result.code ?? 0,
    stderr: result.stderr?.toString() ?? '',
    stdout: result.stdout?.toString() ?? '',
  }
}
