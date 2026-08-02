import { afterAll, beforeAll, describe, expect, it } from 'vitest'
/**
 * @file Round-trip injection and extraction tests for binject Tests the
 *   complete workflow:
 *
 *   1. Inject resources (SEA blob, VFS archive) into binary
 *   2. Extract resources using binject extract command
 *   3. Verify extracted data matches original input
 *   4. Validate binary integrity after injection This ensures that:
 *
 *   - Resources are correctly embedded in binary
 *   - Extraction logic correctly reads embedded resources
 *   - No data corruption occurs during inject/extract cycle
 *   - Binary format remains valid after modifications
 */

import { existsSync, promises as fs } from 'node:fs'
import os from 'node:os'
import path from 'node:path'

import { safeDelete, safeMkdir } from '@socketsecurity/lib-stable/fs/safe'
import { getDefaultLogger } from '@socketsecurity/lib-stable/logger/default'

import { getBinjectPath } from './helpers/paths.mts'
import { execCommand, hashFile } from './helpers/exec-command.mts'
import { tolerantTimeout } from '../../../test/fleet/_shared/lib/timing.mts'

const logger = getDefaultLogger()

const BINJECT = getBinjectPath()

let testDir: string
// Resolved at module scope on purpose: `describe.skipIf` is evaluated when
// the suite is REGISTERED, before any beforeAll runs. Assigning this in a
// hook left it false at that moment, so skipIf(!binjectExists) was always
// skipIf(true) and the whole suite silently never ran.
const binjectExists = existsSync(BINJECT)

beforeAll(async () => {
  // Check if binject exists
  if (!binjectExists) {
    logger.warn(`binject not found at ${BINJECT}`)
    logger.warn('   Run: pnpm build in packages/binject')
    return
  }

  // Create test directory
  testDir = path.join(os.tmpdir(), `binject-roundtrip-${Date.now()}`)
  await safeMkdir(testDir)
})

afterAll(async () => {
  if (testDir) {
    await safeDelete(testDir)
  }
})

describe.skipIf(!binjectExists)('round-trip injection and extraction', () => {
  describe('sEA blob round-trip', () => {
    it(
      'should inject and extract SEA blob with identical content',
      async () => {
        // Create test SEA blob
        const seaBlob = path.join(testDir, 'test.blob')
        const seaContent = Buffer.from(`TEST_SEA_BLOB_CONTENT_${Date.now()}`)
        await fs.writeFile(seaBlob, seaContent)

        // Create a placeholder binary, using binject itself as the test binary.
        const inputBinary = path.join(testDir, 'input_binary')
        await fs.copyFile(BINJECT, inputBinary)

        const outputBinary = path.join(testDir, 'output_with_sea')

        // Inject SEA blob
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

        // Extract SEA blob
        const extractedSea = path.join(testDir, 'extracted.blob')
        const extractResult = await execCommand(BINJECT, [
          'extract',
          '-e',
          outputBinary,
          '-o',
          extractedSea,
          '--sea',
        ])

        expect(extractResult.code).toBe(0)
        expect(existsSync(extractedSea)).toBeTruthy()

        // Compare original and extracted
        const originalHash = await hashFile(seaBlob)
        const extractedHash = await hashFile(extractedSea)

        expect(extractedHash).toBe(originalHash)

        // Verify byte-for-byte equality
        const originalContent = await fs.readFile(seaBlob)
        const extractedContent = await fs.readFile(extractedSea)
        expect(Buffer.compare(originalContent, extractedContent)).toBe(0)
      },
      tolerantTimeout(30_000),
    )

    it(
      'should preserve binary functionality after SEA injection',
      async () => {
        const inputBinary = path.join(testDir, 'func_test_input')
        await fs.copyFile(BINJECT, inputBinary)

        const seaBlob = path.join(testDir, 'func_test.blob')
        await fs.writeFile(seaBlob, Buffer.from('test content'))

        const outputBinary = path.join(testDir, 'func_test_output')

        // Inject
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

        // Verify output binary is still executable (test with --help)
        const execResult = await execCommand(outputBinary, ['--help'])
        expect(execResult.code).toBe(0)
        expect(execResult.stdout).toContain('binject')
      },
      tolerantTimeout(30_000),
    )

    it(
      'should handle large SEA blobs (>1MB)',
      async () => {
        // Create 2MB SEA blob
        const seaBlob = path.join(testDir, 'large.blob')
        const largeContent = Buffer.alloc(2 * 1024 * 1024)
        // Fill with pattern for compression resistance
        for (let i = 0; i < largeContent.length; i++) {
          largeContent[i] = i % 256
        }
        await fs.writeFile(seaBlob, largeContent)

        const inputBinary = path.join(testDir, 'large_input')
        await fs.copyFile(BINJECT, inputBinary)

        const outputBinary = path.join(testDir, 'large_output')

        // Inject
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

        // Extract
        const extractedSea = path.join(testDir, 'large_extracted.blob')
        const extractResult = await execCommand(BINJECT, [
          'extract',
          '-e',
          outputBinary,
          '-o',
          extractedSea,
          '--sea',
        ])

        expect(extractResult.code).toBe(0)

        // Verify size and hash
        // oxlint-disable-next-line socket/prefer-exists-sync -- need stats.size to verify extracted payload matches.
        const extractedStats = await fs.stat(extractedSea)
        expect(extractedStats.size).toBe(largeContent.length)

        const originalHash = await hashFile(seaBlob)
        const extractedHash = await hashFile(extractedSea)
        expect(extractedHash).toBe(originalHash)
      },
      tolerantTimeout(60_000),
    )
  })

  describe('vFS archive round-trip', () => {
    it(
      'should inject and extract VFS archive with identical content',
      async () => {
        // Create test VFS archive
        const vfsArchive = path.join(testDir, 'test.vfs')
        const vfsContent = Buffer.from(`TEST_VFS_ARCHIVE_CONTENT_${Date.now()}`)
        await fs.writeFile(vfsArchive, vfsContent)

        // Create test SEA blob (required for VFS injection)
        const seaBlob = path.join(testDir, 'vfs_test.blob')
        await fs.writeFile(seaBlob, Buffer.from('test'))

        const inputBinary = path.join(testDir, 'vfs_input')
        await fs.copyFile(BINJECT, inputBinary)

        const outputBinary = path.join(testDir, 'vfs_output')

        // Inject VFS (with required SEA blob)
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

        // Extract VFS
        const extractedVfs = path.join(testDir, 'extracted.vfs')
        const extractResult = await execCommand(BINJECT, [
          'extract',
          '-e',
          outputBinary,
          '-o',
          extractedVfs,
          '--vfs',
        ])

        expect(extractResult.code).toBe(0)

        // Compare
        const originalHash = await hashFile(vfsArchive)
        const extractedHash = await hashFile(extractedVfs)
        expect(extractedHash).toBe(originalHash)
      },
      tolerantTimeout(30_000),
    )

    it(
      'should handle VFS with special characters in filename',
      async () => {
        const vfsArchive = path.join(testDir, 'test-vfs_v1.2.3.vfs')
        const vfsContent = Buffer.from('VFS_CONTENT_WITH_SPECIAL_NAME')
        await fs.writeFile(vfsArchive, vfsContent)

        // Create test SEA blob (required for VFS injection)
        const seaBlob = path.join(testDir, 'special_test.blob')
        await fs.writeFile(seaBlob, Buffer.from('test'))

        const inputBinary = path.join(testDir, 'special_input')
        await fs.copyFile(BINJECT, inputBinary)

        const outputBinary = path.join(testDir, 'special_output')

        // Inject (with required SEA blob)
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

        // Extract
        const extractedVfs = path.join(testDir, 'special_extracted.vfs')
        const extractResult = await execCommand(BINJECT, [
          'extract',
          '-e',
          outputBinary,
          '-o',
          extractedVfs,
          '--vfs',
        ])

        expect(extractResult.code).toBe(0)

        // Verify
        const originalContent = await fs.readFile(vfsArchive)
        const extractedContent = await fs.readFile(extractedVfs)
        expect(Buffer.compare(originalContent, extractedContent)).toBe(0)
      },
      tolerantTimeout(30_000),
    )
  })
})
