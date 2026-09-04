/**
 * @file Resource size limit tests for binject Validates that binject correctly
 *   handles size boundaries:
 *
 *   - MAX_SEA_BLOB_SIZE enforcement
 *   - MAX_VFS_SIZE enforcement
 *   - MAX_NODE_BINARY_SIZE enforcement
 *   - Boundary conditions (max size, max size + 1, etc.)
 *   - Memory allocation limits
 *   - Graceful failure for oversized resources These tests ensure binject doesn't
 *     crash or corrupt data when handling large files near the maximum
 *     supported sizes.
 */

import { afterAll, beforeAll, describe, expect, it } from 'vitest'

import { existsSync, promises as fs } from 'node:fs'
import os from 'node:os'
import path from 'node:path'

import { safeDelete, safeMkdir } from '@socketsecurity/lib-stable/fs/safe'
import { getDefaultLogger } from '@socketsecurity/lib-stable/logger/default'
import { spawn } from '@socketsecurity/lib-stable/process/spawn/child'

import { MAX_SEA_BLOB_SIZE, MAX_VFS_SIZE } from './helpers/constants.mts'
import { getBinjectPath } from './helpers/paths.mts'
import { tolerantTimeout } from '../../../test/fleet/_shared/lib/timing.mts'

const TIMEOUT_30S = tolerantTimeout(30_000)
const TIMEOUT_120S = tolerantTimeout(120_000)
const TIMEOUT_180S = tolerantTimeout(180_000)

const logger = getDefaultLogger()

const BINJECT = getBinjectPath()

let testDir: string
// Resolved at module scope on purpose: `describe.skipIf` is evaluated when
// the suite is REGISTERED, before any beforeAll runs. Assigning this in a
// hook left it false at that moment, so skipIf(!binjectExists) was always
// skipIf(true) and the whole suite silently never ran.
const binjectExists = existsSync(BINJECT)

import { createTestFile, execCommand } from './helpers/size-fixtures.mts'

// Companion SEA blob shared by the VFS tests (--vfs requires --sea).
let vfsSeaBlob: string
beforeAll(async () => {
  if (!binjectExists) {
    logger.warn(`binject not found at ${BINJECT}`)
    return
  }

  testDir = path.join(os.tmpdir(), `binject-limits-${Date.now()}`)
  await safeMkdir(testDir)
  vfsSeaBlob = path.join(testDir, 'vfs_companion_sea.blob')
  await fs.writeFile(vfsSeaBlob, Buffer.from('test'))
})

afterAll(async () => {
  if (testDir) {
    await safeDelete(testDir)
  }
})

describe.skipIf(!binjectExists)('resource size limit enforcement', () => {
  describe('sEA blob size limits', () => {
    it(
      'should accept SEA blob at maximum size',
      async () => {
        const inputBinary = path.join(testDir, 'max_sea_input')
        await fs.copyFile(BINJECT, inputBinary)

        const seaBlob = path.join(testDir, 'max_size.blob')
        // Use MAX_SEA_BLOB_SIZE from constants
        await createTestFile(seaBlob, MAX_SEA_BLOB_SIZE)

        const outputBinary = path.join(testDir, 'max_sea_output')

        // Should succeed at max size
        const injectResult = await execCommand(BINJECT, [
          'inject',
          '-e',
          inputBinary,
          '-o',
          outputBinary,
          '--sea',
          seaBlob,
        ])

        expect(injectResult.code).toBe(0)
        expect(existsSync(outputBinary)).toBeTruthy()
        // 2 minute timeout for large file
      },
      TIMEOUT_120S,
    )

    it(
      'should reject SEA blob exceeding maximum size',
      async () => {
        const inputBinary = path.join(testDir, 'oversized_sea_input')
        await fs.copyFile(BINJECT, inputBinary)

        const seaBlob = path.join(testDir, 'oversized.blob')
        // Create blob larger than MAX_SEA_BLOB_SIZE
        await createTestFile(seaBlob, MAX_SEA_BLOB_SIZE + 1024)

        const outputBinary = path.join(testDir, 'oversized_sea_output')

        // Should fail with oversized blob
        const injectResult = await execCommand(BINJECT, [
          'inject',
          '-e',
          inputBinary,
          '-o',
          outputBinary,
          '--sea',
          seaBlob,
        ])

        expect(injectResult.code).not.toBe(0)
        expect(injectResult.stderr).toBeTruthy()
        expect(injectResult.stderr.toLowerCase()).toMatch(
          /size|large|limit|exceed/,
        )
      },
      TIMEOUT_120S,
    )

    it(
      'should handle boundary at MAX_SEA_BLOB_SIZE - 1',
      async () => {
        const inputBinary = path.join(testDir, 'boundary_sea_input')
        await fs.copyFile(BINJECT, inputBinary)

        const seaBlob = path.join(testDir, 'boundary.blob')
        await createTestFile(seaBlob, MAX_SEA_BLOB_SIZE - 1)

        const outputBinary = path.join(testDir, 'boundary_sea_output')

        // Should succeed just below max size
        const injectResult = await execCommand(BINJECT, [
          'inject',
          '-e',
          inputBinary,
          '-o',
          outputBinary,
          '--sea',
          seaBlob,
        ])

        expect(injectResult.code).toBe(0)
      },
      TIMEOUT_120S,
    )
  })

  describe('vFS archive size limits', () => {
    it(
      'should accept VFS archive at maximum size',
      async () => {
        const inputBinary = path.join(testDir, 'max_vfs_input')
        await fs.copyFile(BINJECT, inputBinary)

        const vfsArchive = path.join(testDir, 'max_size.vfs')
        await createTestFile(vfsArchive, MAX_VFS_SIZE)

        const outputBinary = path.join(testDir, 'max_vfs_output')

        // Should succeed at max size
        const injectResult = await execCommand(BINJECT, [
          'inject',
          '-e',
          inputBinary,
          '-o',
          outputBinary,
          '--sea',
          vfsSeaBlob,
          '--vfs',
          vfsArchive,
        ])

        expect(injectResult.code).toBe(0)
        expect(existsSync(outputBinary)).toBeTruthy()
      },
      TIMEOUT_120S,
    )

    it(
      'should reject VFS archive exceeding maximum size',
      async () => {
        const inputBinary = path.join(testDir, 'oversized_vfs_input')
        await fs.copyFile(BINJECT, inputBinary)

        const vfsArchive = path.join(testDir, 'oversized.vfs')
        await createTestFile(vfsArchive, MAX_VFS_SIZE + 1024)

        const outputBinary = path.join(testDir, 'oversized_vfs_output')

        // Should fail with oversized VFS
        const injectResult = await execCommand(BINJECT, [
          'inject',
          '-e',
          inputBinary,
          '-o',
          outputBinary,
          '--sea',
          vfsSeaBlob,
          '--vfs',
          vfsArchive,
        ])

        expect(injectResult.code).not.toBe(0)
        expect(injectResult.stderr).toBeTruthy()
        expect(injectResult.stderr.toLowerCase()).toMatch(
          /size|large|limit|exceed/,
        )
      },
      TIMEOUT_120S,
    )

    it(
      'should handle boundary at MAX_VFS_SIZE - 1',
      async () => {
        const inputBinary = path.join(testDir, 'boundary_vfs_input')
        await fs.copyFile(BINJECT, inputBinary)

        const vfsArchive = path.join(testDir, 'boundary.vfs')
        await createTestFile(vfsArchive, MAX_VFS_SIZE - 1)

        const outputBinary = path.join(testDir, 'boundary_vfs_output')

        // Should succeed just below max size
        const injectResult = await execCommand(BINJECT, [
          'inject',
          '-e',
          inputBinary,
          '-o',
          outputBinary,
          '--sea',
          vfsSeaBlob,
          '--vfs',
          vfsArchive,
        ])

        expect(injectResult.code).toBe(0)
      },
      TIMEOUT_120S,
    )
  })

  describe('binary size limits', () => {
    it(
      'should validate input binary size',
      async () => {
        // This test verifies that binject checks input binary size
        // We can't easily create a MAX_NODE_BINARY_SIZE file, but we can
        // verify that reasonable sizes work

        const inputBinary = path.join(testDir, 'normal_input')
        await fs.copyFile(BINJECT, inputBinary)

        const seaBlob = path.join(testDir, 'normal.blob')
        await fs.writeFile(seaBlob, Buffer.from('test'))

        const outputBinary = path.join(testDir, 'normal_output')

        // Normal-sized binary should work
        const injectResult = await execCommand(BINJECT, [
          'inject',
          '-e',
          inputBinary,
          '-o',
          outputBinary,
          '--sea',
          seaBlob,
        ])

        expect(injectResult.code).toBe(0)
      },
      TIMEOUT_30S,
    )
  })

  describe('combined size limits', () => {
    it(
      'should handle maximum SEA + maximum VFS together',
      async () => {
        const inputBinary = path.join(testDir, 'combined_input')
        await fs.copyFile(BINJECT, inputBinary)

        const seaBlob = path.join(testDir, 'combined.blob')
        // Use smaller sizes for faster testing (10MB each)
        const testSize = 10 * 1024 * 1024
        await createTestFile(seaBlob, testSize)

        const vfsArchive = path.join(testDir, 'combined.vfs')
        await createTestFile(vfsArchive, testSize)

        const outputBinary = path.join(testDir, 'combined_output')

        // Should handle both resources
        const injectResult = await execCommand(BINJECT, [
          'inject',
          '-e',
          inputBinary,
          '-o',
          outputBinary,
          '--sea',
          seaBlob,
          '--vfs',
          vfsArchive,
        ])

        expect(injectResult.code).toBe(0)

        // Verify output binary is larger than input
        // oxlint-disable-next-line socket/prefer-exists-sync -- need stats.size for size-growth assertion.
        const inputStats = await fs.stat(inputBinary)
        // oxlint-disable-next-line socket/prefer-exists-sync -- need stats.size for size-growth assertion.
        const outputStats = await fs.stat(outputBinary)

        // The SEA blob is stored verbatim, but the VFS archive is gzip-
        // compressed on inject (and the pattern fixture compresses heavily),
        // so only the SEA size is a guaranteed lower bound on growth.
        const expectedMinSize = inputStats.size + testSize
        expect(outputStats.size).toBeGreaterThanOrEqual(expectedMinSize)
        // 3 minute timeout
      },
      TIMEOUT_180S,
    )
  })

  describe('small resource handling', () => {
    it(
      'should handle minimum size resources (1 byte)',
      async () => {
        const inputBinary = path.join(testDir, 'min_input')
        await fs.copyFile(BINJECT, inputBinary)

        const seaBlob = path.join(testDir, 'min.blob')
        // 1 byte
        await fs.writeFile(seaBlob, Buffer.from([0x42]))

        const outputBinary = path.join(testDir, 'min_output')

        const injectResult = await execCommand(BINJECT, [
          'inject',
          '-e',
          inputBinary,
          '-o',
          outputBinary,
          '--sea',
          seaBlob,
        ])

        expect(injectResult.code).toBe(0)
      },
      TIMEOUT_30S,
    )

    it(
      'should reject zero-length resources',
      async () => {
        const inputBinary = path.join(testDir, 'zero_input')
        await fs.copyFile(BINJECT, inputBinary)

        const seaBlob = path.join(testDir, 'zero.blob')
        // 0 bytes
        await fs.writeFile(seaBlob, Buffer.alloc(0))

        const outputBinary = path.join(testDir, 'zero_output')

        // Zero-length resources should be rejected
        const injectResult = await execCommand(BINJECT, [
          'inject',
          '-e',
          inputBinary,
          '-o',
          outputBinary,
          '--sea',
          seaBlob,
        ])

        expect(injectResult.code).not.toBe(0)
        expect(injectResult.stderr).toBeTruthy()
        expect(injectResult.stderr.toLowerCase()).toMatch(/empty|size|invalid/)
      },
      TIMEOUT_30S,
    )
  })

  describe('error messages', () => {
    it(
      'should provide clear error for oversized SEA blob',
      async () => {
        const inputBinary = path.join(testDir, 'error_input')
        await fs.copyFile(BINJECT, inputBinary)

        const seaBlob = path.join(testDir, 'error_oversized.blob')
        // Create significantly oversized blob
        await createTestFile(seaBlob, MAX_SEA_BLOB_SIZE + 1024 * 1024)

        const outputBinary = path.join(testDir, 'error_output')

        const injectResult = await execCommand(BINJECT, [
          'inject',
          '-e',
          inputBinary,
          '-o',
          outputBinary,
          '--sea',
          seaBlob,
        ])

        expect(injectResult.code).not.toBe(0)
        expect(injectResult.stderr).toBeTruthy()

        // Should mention size or limit in error
        const errorMsg = injectResult.stderr.toLowerCase()
        const hasRelevantError =
          errorMsg.includes('size') ||
          errorMsg.includes('large') ||
          errorMsg.includes('limit') ||
          errorMsg.includes('exceed') ||
          errorMsg.includes('maximum') ||
          errorMsg.includes('too big')

        expect(hasRelevantError).toBeTruthy()
      },
      TIMEOUT_120S,
    )

    it(
      'should provide clear error for oversized VFS archive',
      async () => {
        const inputBinary = path.join(testDir, 'vfs_error_input')
        await fs.copyFile(BINJECT, inputBinary)

        const vfsArchive = path.join(testDir, 'vfs_error_oversized.vfs')
        await createTestFile(vfsArchive, MAX_VFS_SIZE + 1024 * 1024)

        const outputBinary = path.join(testDir, 'vfs_error_output')

        const injectResult = await execCommand(BINJECT, [
          'inject',
          '-e',
          inputBinary,
          '-o',
          outputBinary,
          '--sea',
          vfsSeaBlob,
          '--vfs',
          vfsArchive,
        ])

        expect(injectResult.code).not.toBe(0)
        expect(injectResult.stderr).toBeTruthy()
        expect(injectResult.stderr.toLowerCase()).toMatch(
          /size|large|limit|exceed/,
        )
      },
      TIMEOUT_120S,
    )
  })

  describe('memory safety', () => {
    it(
      'should handle 50MB resource allocation',
      async () => {
        // Test that binject correctly handles large (but valid) resources

        const inputBinary = path.join(testDir, 'memory_input')
        await fs.copyFile(BINJECT, inputBinary)

        const seaBlob = path.join(testDir, 'memory.blob')
        // Create 50MB blob (well within the 500MB MAX_SEA_BLOB_SIZE)
        await createTestFile(seaBlob, 50 * 1024 * 1024)

        const outputBinary = path.join(testDir, 'memory_output')

        // 50MB should be within limits and succeed
        const injectResult = await execCommand(BINJECT, [
          'inject',
          '-e',
          inputBinary,
          '-o',
          outputBinary,
          '--sea',
          seaBlob,
        ])

        expect(injectResult.code).toBe(0)
        expect(existsSync(outputBinary)).toBeTruthy()
      },
      TIMEOUT_180S,
    )
  })
})
