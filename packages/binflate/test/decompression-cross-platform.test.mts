import { afterAll, beforeAll, describe, expect, it } from 'vitest'
/**
 * @file Cross-platform Binflate decompression functional tests
 *   Tests the complete decompression workflow:
 *
 *   1. Use binpress to compress a binary
 *   2. Use binflate to decompress it
 *   3. Verify decompressed binary matches original
 *   4. Test error handling for corrupt/invalid data
 *   5. Validate CLI flags and options These tests ensure:
 *
 *   - Decompression produces byte-identical output
 *   - zstd decompression works correctly
 *   - Error handling for corrupt data
 *   - CLI flags work as documented
 *   - Cross-platform compatibility CRITICAL: These are P0 tests - binflate ships
 *     with NO functional tests without this file. The shell tests only validate
 *     binary structure.
 */

import crypto from 'node:crypto'
import { existsSync, promises as fs } from 'node:fs'
import os from 'node:os'
import path from 'node:path'
import process from 'node:process'

import { fileURLToPath } from 'node:url'

import { makeExecutable } from 'build-infra/lib/build-helpers'
import { getBuildMode } from 'build-infra/lib/constants'
import { getCurrentPlatformArch } from 'build-infra/lib/platform-mappings'

import { safeDelete, safeMkdir } from '@socketsecurity/lib-stable/fs/safe'
import { getDefaultLogger } from '@socketsecurity/lib-stable/logger/default'

import {
  createTestBinary,
  execCommand,
  hashFile,
} from './helpers/decompression-functional.mts'
import { tolerantTimeout } from '../../../test/fleet/_shared/lib/timing.mts'

const logger = getDefaultLogger()

const __filename = fileURLToPath(import.meta.url)
const __dirname = path.dirname(__filename)
const PACKAGE_DIR = path.join(__dirname, '..')
const PACKAGES_DIR = path.join(PACKAGE_DIR, '..')
const BINPRESS_PACKAGE_DIR = path.join(PACKAGES_DIR, 'binpress')

// Determine build mode + per-platform-arch layout.
const BUILD_MODE = getBuildMode()
// socket-lint: allow top-level-await -- vitest ESM test file, never bundled to CJS
const PLATFORM_ARCH = await getCurrentPlatformArch()

// Get binflate binary path
const BINFLATE_NAME = process.platform === 'win32' ? 'binflate.exe' : 'binflate'
const BINFLATE = path.join(
  PACKAGE_DIR,
  'build',
  BUILD_MODE,
  PLATFORM_ARCH,
  'out',
  'Final',
  BINFLATE_NAME,
)

// Get binpress binary path (needed for creating test data)
const BINPRESS_NAME = process.platform === 'win32' ? 'binpress.exe' : 'binpress'
const BINPRESS = path.join(
  BINPRESS_PACKAGE_DIR,
  'build',
  BUILD_MODE,
  PLATFORM_ARCH,
  'out',
  'Final',
  BINPRESS_NAME,
)

let testDir: string
let testBinary: string

// Use Node.js binary as consistent test input (not BINFLATE/BINPRESS which may vary)
const NODE_BINARY = process.execPath

// Check if binaries exist (done at module load time for skipIf)
const binflateExists = existsSync(BINFLATE)
const binpressExists = existsSync(BINPRESS)

beforeAll(async () => {
  if (!binflateExists) {
    logger.warn(`binflate not found at ${BINFLATE}`)
    logger.warn('   Run: pnpm build in packages/binflate')
    return
  }

  if (!binpressExists) {
    logger.warn(`binpress not found at ${BINPRESS}`)
    logger.warn('   Run: pnpm build in packages/binpress')
    logger.warn('   binpress is required to create compressed test data')
    return
  }

  // Create unique test directory with timestamp and random suffix to isolate from parallel runs
  const uniqueId = crypto.randomUUID()
  testDir = path.join(os.tmpdir(), `binflate-functional-${uniqueId}`)
  await safeMkdir(testDir)

  // Copy Node.js binary as consistent test input (not BINFLATE/BINPRESS which may vary between builds)
  testBinary = path.join(testDir, 'test-node')
  await fs.copyFile(NODE_BINARY, testBinary)
  await makeExecutable(testBinary)
})

afterAll(async () => {
  if (testDir) {
    await safeDelete(testDir)
  }
})

describe.skipIf(!binflateExists || !binpressExists)(
  'binflate decompression functional tests',
  () => {
    describe('cross-platform compatibility', () => {
      it(
        'should decompress binaries on current platform',
        async () => {
          // Use consistent test binary
          const originalBinary = testBinary

          // Compress
          const compressedBinary = path.join(testDir, 'platform_compressed')
          await execCommand(BINPRESS, [originalBinary, '-o', compressedBinary])

          // Decompress
          const decompressedBinary = path.join(testDir, 'platform_decompressed')
          const result = await execCommand(BINFLATE, [
            compressedBinary,
            '--output',
            decompressedBinary,
          ])

          expect(result.code).toBe(0)

          // Verify platform-specific details
          // oxlint-disable-next-line socket/prefer-exists-sync -- need stats.size to verify decompressed payload is non-empty.
          const stats = await fs.stat(decompressedBinary)
          expect(stats.size).toBeGreaterThan(0)

          // Verify hash match
          const originalHash = await hashFile(originalBinary)
          const decompressedHash = await hashFile(decompressedBinary)
          expect(decompressedHash).toBe(originalHash)
        },
        tolerantTimeout(60_000),
      )
    })
  },
)
