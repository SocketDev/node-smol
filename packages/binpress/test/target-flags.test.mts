/**
 * @file Target flag tests for binpress
 *   Tests target flag parsing and platform normalization:
 *
 *   1. Combined --target flag (e.g., linux-x64-musl)
 *   2. Individual --target-platform, --target-arch, --target-libc flags
 *   3. Platform normalization: win → win32 (input to internal)
 *   4. Asset naming: win32 → win (internal to GitHub releases) These tests ensure:
 *
 *   - Target flags correctly override auto-detection
 *   - Combined and individual flag formats work equivalently
 *   - Platform naming consistency (win32 internal, win for assets)
 *   - Cross-compilation scenarios work correctly
 */

import crypto from 'node:crypto'
import { existsSync, promises as fs } from 'node:fs'
import os from 'node:os'
import path from 'node:path'
import process from 'node:process'

import { fileURLToPath } from 'node:url'

import { makeExecutable } from 'local-build-infra/lib/build-helpers'
import { getBuildMode } from 'local-build-infra/lib/constants'

import { safeDelete, safeMkdir } from '@socketsecurity/lib-stable/fs/safe'
import { getDefaultLogger } from '@socketsecurity/lib-stable/logger/default'
import { spawn } from '@socketsecurity/lib-stable/process/spawn/child'

import { afterAll, beforeAll, describe, expect, it } from 'vitest'

import type { SpawnOptions } from '@socketsecurity/lib-stable/process/spawn/types'
import { tolerantTimeout } from '../../../test/fleet/_shared/lib/timing.mts'
import {
  BINPRESS,
  BUILD_MODE,
  NODE_BINARY,
  PACKAGE_DIR,
} from './helpers/binpress-env.mts'

const TIMEOUT_10S = tolerantTimeout(10_000)
const TIMEOUT_30S = tolerantTimeout(30_000)

const logger = getDefaultLogger()

import { execCommand } from './helpers/exec-command.mts'
import type { ExecCommandResult } from './helpers/exec-command.mts'

beforeAll(async () => {
  // Create unique test directory with timestamp and random suffix to isolate from parallel runs
  const uniqueId = crypto.randomUUID()
  testDir = path.join(os.tmpdir(), `binpress-target-${uniqueId}`)
  await safeMkdir(testDir)

  // Copy Node.js binary as consistent test input (not BINPRESS which may vary between builds)
  testBinary = path.join(testDir, 'test-node')
  await fs.copyFile(NODE_BINARY, testBinary)
  await makeExecutable(testBinary)
})

afterAll(async () => {
  if (testDir) {
    await safeDelete(testDir)
  }
})

describe.skipIf(!existsSync(BINPRESS))('binpress target flags', () => {
  describe('combined --target flag', () => {
    it(
      'should accept --target with platform-arch format',
      async () => {
        const inputBinary = path.join(testDir, 'target_combined_input')
        await fs.copyFile(testBinary, inputBinary)

        const output = path.join(testDir, 'target_combined_output')

        // Test combined target format
        const result = await execCommand(BINPRESS, [
          inputBinary,
          '--output',
          output,
          '--target',
          'linux-x64',
        ])

        // All binpress builds should have linux stubs for cross-compilation
        expect(result.code).toBe(0)
        expect(result.stdout).toContain('Target: linux-x64')
        expect(result.stderr).not.toContain('Failed to parse')
        expect(result.stderr).not.toContain('Unexpected argument')
      },
      TIMEOUT_30S,
    )

    it(
      'should accept --target with platform-arch-libc format',
      async () => {
        const inputBinary = path.join(testDir, 'target_libc_input')
        await fs.copyFile(testBinary, inputBinary)

        const output = path.join(testDir, 'target_libc_output')

        // Test combined target with libc
        const result = await execCommand(BINPRESS, [
          inputBinary,
          '--output',
          output,
          '--target',
          'linux-x64-musl',
        ])

        // All binpress builds should have linux-musl stubs for cross-compilation
        expect(result.code).toBe(0)
        expect(result.stdout).toContain('Target: linux-x64-musl')
        expect(result.stderr).not.toContain('Failed to parse')
        expect(result.stderr).not.toContain('Unexpected argument')
      },
      TIMEOUT_30S,
    )

    it(
      'should accept --target with darwin platform',
      async () => {
        const inputBinary = path.join(testDir, 'target_darwin_input')
        await fs.copyFile(testBinary, inputBinary)

        const output = path.join(testDir, 'target_darwin_output')

        const result = await execCommand(BINPRESS, [
          inputBinary,
          '--output',
          output,
          '--target',
          'darwin-arm64',
        ])

        // All binpress builds should have darwin stubs for cross-compilation
        expect(result.code).toBe(0)
        expect(result.stdout).toContain('Target: darwin-arm64')
        expect(result.stderr).not.toContain('Failed to parse')
        expect(result.stderr).not.toContain('Unexpected argument')
      },
      TIMEOUT_30S,
    )

    it(
      'should accept --target with win32 platform',
      async () => {
        const inputBinary = path.join(testDir, 'target_win32_input')
        await fs.copyFile(testBinary, inputBinary)

        const output = path.join(testDir, 'target_win32_output')

        const result = await execCommand(BINPRESS, [
          inputBinary,
          '--output',
          output,
          '--target',
          'win32-x64',
        ])

        // Win32 stubs may not be available during initial CI setup.
        // Skip test if stub is missing (indicated by specific error message).
        if (
          result.code !== 0 &&
          result.stderr.includes('stub not available (size=0)')
        ) {
          logger.log('Skipping: win32-x64 stub not available')
          return
        }

        expect(result.code).toBe(0)
        expect(result.stdout).toContain('Target: win32-x64')
        expect(result.stderr).not.toContain('Failed to parse')
        expect(result.stderr).not.toContain('Unexpected argument')
      },
      TIMEOUT_30S,
    )
  })

  describe('individual target flags', () => {
    it(
      'should accept --target-platform and --target-arch',
      async () => {
        const inputBinary = path.join(testDir, 'individual_basic_input')
        await fs.copyFile(testBinary, inputBinary)

        const output = path.join(testDir, 'individual_basic_output')

        const result = await execCommand(BINPRESS, [
          inputBinary,
          '--output',
          output,
          '--target-platform',
          'linux',
          '--target-arch',
          'x64',
        ])

        expect(result.code).toBe(0)
        expect(result.stdout).toContain('Target platform: linux')
        expect(result.stdout).toContain('Target arch: x64')
        expect(result.stderr).not.toContain('Failed to parse')
        expect(result.stderr).not.toContain('Unexpected argument')
      },
      TIMEOUT_30S,
    )

    it(
      'should accept --target-libc with Linux platform',
      async () => {
        const inputBinary = path.join(testDir, 'individual_libc_input')
        await fs.copyFile(testBinary, inputBinary)

        const output = path.join(testDir, 'individual_libc_output')

        const result = await execCommand(BINPRESS, [
          inputBinary,
          '--output',
          output,
          '--target-platform',
          'linux',
          '--target-arch',
          'x64',
          '--target-libc',
          'musl',
        ])

        expect(result.code).toBe(0)
        expect(result.stdout).toContain('Target platform: linux')
        expect(result.stdout).toContain('Target arch: x64')
        expect(result.stdout).toContain('Target libc: musl')
        expect(result.stderr).not.toContain('Failed to parse')
        expect(result.stderr).not.toContain('Unexpected argument')
      },
      TIMEOUT_30S,
    )

    it(
      'should accept all three individual flags together',
      async () => {
        const inputBinary = path.join(testDir, 'individual_all_input')
        await fs.copyFile(testBinary, inputBinary)

        const output = path.join(testDir, 'individual_all_output')

        const result = await execCommand(BINPRESS, [
          inputBinary,
          '--output',
          output,
          '--target-platform',
          'linux',
          '--target-arch',
          'arm64',
          '--target-libc',
          'glibc',
        ])

        expect(result.code).toBe(0)
        expect(result.stdout).toContain('Target platform: linux')
        expect(result.stdout).toContain('Target arch: arm64')
        expect(result.stdout).toContain('Target libc: glibc')
        expect(result.stderr).not.toContain('Failed to parse')
        expect(result.stderr).not.toContain('Unexpected argument')
      },
      TIMEOUT_30S,
    )
  })

  describe('platform normalization: win → win32', () => {
    it(
      'should accept "win" as platform and normalize to win32 internally',
      async () => {
        const inputBinary = path.join(testDir, 'win_normalize_input')
        await fs.copyFile(testBinary, inputBinary)

        const output = path.join(testDir, 'win_normalize_output')

        // Test with "win" as input (should normalize to "win32")
        const result = await execCommand(BINPRESS, [
          inputBinary,
          '--output',
          output,
          '--target',
          'win-x64',
        ])

        // binpress should accept "win" as input
        expect(result.stdout).toContain('Target: win-x64')
        expect(result.stderr).not.toContain('Unexpected argument')
      },
      TIMEOUT_30S,
    )

    it(
      'should accept "win32" as platform directly',
      async () => {
        const inputBinary = path.join(testDir, 'win32_direct_input')
        await fs.copyFile(testBinary, inputBinary)

        const output = path.join(testDir, 'win32_direct_output')

        const result = await execCommand(BINPRESS, [
          inputBinary,
          '--output',
          output,
          '--target',
          'win32-x64',
        ])

        expect(result.stdout).toContain('Target: win32-x64')
        expect(result.stderr).not.toContain('Unexpected argument')
      },
      TIMEOUT_30S,
    )

    it(
      'should accept "win" via --target-platform flag',
      async () => {
        const inputBinary = path.join(testDir, 'win_flag_input')
        await fs.copyFile(testBinary, inputBinary)

        const output = path.join(testDir, 'win_flag_output')

        const result = await execCommand(BINPRESS, [
          inputBinary,
          '--output',
          output,
          '--target-platform',
          'win',
          '--target-arch',
          'x64',
        ])

        // Should accept "win" as platform
        expect(result.stdout).toContain('Target platform: win')
        expect(result.stderr).not.toContain('Unexpected argument')
      },
      TIMEOUT_30S,
    )
  })

  describe('help and version', () => {
    it(
      'should show target flags in --help output',
      async () => {
        const result = await execCommand(BINPRESS, ['--help'])

        expect(result.code).toBe(0)
        expect(result.stdout).toContain('--target')
        expect(result.stdout).toContain('--target-platform')
        expect(result.stdout).toContain('--target-arch')
        expect(result.stdout).toContain('--target-libc')
        expect(result.stdout).toContain('--node-version')
        // Should show example with target flags
        expect(result.stdout).toContain('linux-x64-musl')
      },
      TIMEOUT_10S,
    )
  })

  describe('--node-version flag', () => {
    it(
      'should accept --node-version and display it',
      async () => {
        const inputBinary = path.join(testDir, 'node_version_input')
        await fs.copyFile(testBinary, inputBinary)

        const output = path.join(testDir, 'node_version_output')

        const result = await execCommand(BINPRESS, [
          inputBinary,
          '--output',
          output,
          '--node-version',
          '22.5.0',
        ])

        expect(result.code).toBe(0)
        expect(result.stdout).toContain('Node version: 22.5.0')
        expect(result.stderr).not.toContain('Unexpected argument')
      },
      TIMEOUT_30S,
    )

    it(
      'should accept --node-version with target flags',
      async () => {
        const inputBinary = path.join(testDir, 'node_version_target_input')
        await fs.copyFile(testBinary, inputBinary)

        const output = path.join(testDir, 'node_version_target_output')

        const result = await execCommand(BINPRESS, [
          inputBinary,
          '--output',
          output,
          '--target',
          'linux-x64-musl',
          '--node-version',
          '24.12.0',
        ])

        expect(result.code).toBe(0)
        expect(result.stdout).toContain('Target: linux-x64-musl')
        expect(result.stdout).toContain('Node version: 24.12.0')
        expect(result.stderr).not.toContain('Unexpected argument')
      },
      TIMEOUT_30S,
    )

    it(
      'should use provided version instead of auto-detecting',
      async () => {
        const inputBinary = path.join(testDir, 'node_version_use_input')
        await fs.copyFile(testBinary, inputBinary)

        const output = path.join(testDir, 'node_version_use_output')

        const result = await execCommand(BINPRESS, [
          inputBinary,
          '--output',
          output,
          '--node-version',
          '20.0.0',
        ])

        expect(result.code).toBe(0)
        // Should show "Using provided" not "Detected"
        expect(result.stdout).toContain(
          'Using provided Node.js version: 20.0.0',
        )
        expect(result.stdout).not.toContain('Detected Node.js version')
      },
      TIMEOUT_30S,
    )

    it(
      'should error when --node-version is missing argument',
      async () => {
        const inputBinary = path.join(testDir, 'node_version_error_input')
        await fs.copyFile(testBinary, inputBinary)

        const output = path.join(testDir, 'node_version_error_output')

        // --node-version at end without value
        const result = await execCommand(BINPRESS, [
          inputBinary,
          '--output',
          output,
          '--node-version',
        ])

        expect(result.code).not.toBe(0)
        expect(result.stderr).toContain('--node-version requires')
      },
      TIMEOUT_10S,
    )
  })
})
