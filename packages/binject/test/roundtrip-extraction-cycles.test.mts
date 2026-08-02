import { afterAll, beforeAll, describe, expect, it } from 'vitest'
/**
 * @file Round-trip (batch + repeated cycles) injection and extraction tests for
 *   binject Tests the complete workflow:
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
  describe('batch injection round-trip', () => {
    it(
      'should inject and extract both SEA and VFS with identical content',
      async () => {
        // Create resources
        const seaBlob = path.join(testDir, 'batch.blob')
        const seaContent = Buffer.from('BATCH_SEA_CONTENT')
        await fs.writeFile(seaBlob, seaContent)

        const vfsArchive = path.join(testDir, 'batch.vfs')
        const vfsContent = Buffer.from('BATCH_VFS_CONTENT')
        await fs.writeFile(vfsArchive, vfsContent)

        const inputBinary = path.join(testDir, 'batch_input')
        await fs.copyFile(BINJECT, inputBinary)

        const outputBinary = path.join(testDir, 'batch_output')

        // Inject both
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

        // Extract SEA
        const extractedSea = path.join(testDir, 'batch_extracted.blob')
        const seaExtractResult = await execCommand(BINJECT, [
          'extract',
          '-e',
          outputBinary,
          '-o',
          extractedSea,
          '--sea',
        ])

        expect(seaExtractResult.code).toBe(0)

        // Extract VFS
        const extractedVfs = path.join(testDir, 'batch_extracted.vfs')
        const vfsExtractResult = await execCommand(BINJECT, [
          'extract',
          '-e',
          outputBinary,
          '-o',
          extractedVfs,
          '--vfs',
        ])

        expect(vfsExtractResult.code).toBe(0)

        // Verify both
        const seaOriginalHash = await hashFile(seaBlob)
        const seaExtractedHash = await hashFile(extractedSea)
        expect(seaExtractedHash).toBe(seaOriginalHash)

        const vfsOriginalHash = await hashFile(vfsArchive)
        const vfsExtractedHash = await hashFile(extractedVfs)
        expect(vfsExtractedHash).toBe(vfsOriginalHash)
      },
      tolerantTimeout(30_000),
    )
  })

  describe('multiple injection/extraction cycles', () => {
    it(
      'should maintain integrity after 3 inject/extract cycles',
      async () => {
        let currentBinary = path.join(testDir, 'cycle_input')
        await fs.copyFile(BINJECT, currentBinary)

        const originalSeaBlob = path.join(testDir, 'original.blob')
        const originalContent = Buffer.from('CYCLE_TEST_CONTENT')
        await fs.writeFile(originalSeaBlob, originalContent)

        // 3 cycles of inject + extract
        for (let i = 0; i < 3; i++) {
          const seaBlob = path.join(testDir, `cycle_${i}.blob`)
          // eslint-disable-next-line no-await-in-loop
          await fs.copyFile(originalSeaBlob, seaBlob)

          const outputBinary = path.join(testDir, `cycle_output_${i}`)

          // Inject
          // eslint-disable-next-line no-await-in-loop
          const injectResult = await execCommand(BINJECT, [
            'inject',
            '-e',
            currentBinary,
            '-o',
            outputBinary,
            '--sea',
            seaBlob,
          ])

          expect(injectResult.code).toBe(0)

          // Extract
          const extractedBlob = path.join(testDir, `cycle_extracted_${i}.blob`)
          // eslint-disable-next-line no-await-in-loop
          const extractResult = await execCommand(BINJECT, [
            'extract',
            '-e',
            outputBinary,
            '-o',
            extractedBlob,
            '--sea',
          ])

          expect(extractResult.code).toBe(0)

          // Verify hash matches original
          // eslint-disable-next-line no-await-in-loop
          const extractedHash = await hashFile(extractedBlob)
          // eslint-disable-next-line no-await-in-loop
          const originalHash = await hashFile(originalSeaBlob)
          expect(extractedHash).toBe(originalHash)

          // Update current binary for next cycle
          currentBinary = outputBinary
        }
      },
      tolerantTimeout(90_000),
    )

    it(
      'should handle re-injection with extraction validation',
      async () => {
        const inputBinary = path.join(testDir, 'reinject_input')
        await fs.copyFile(BINJECT, inputBinary)

        const blob1 = path.join(testDir, 'blob1.blob')
        await fs.writeFile(blob1, Buffer.from('FIRST_INJECTION'))

        const blob2 = path.join(testDir, 'blob2.blob')
        await fs.writeFile(blob2, Buffer.from('SECOND_INJECTION'))

        const output1 = path.join(testDir, 'reinject_output1')
        const output2 = path.join(testDir, 'reinject_output2')

        // First injection
        await execCommand(BINJECT, [
          'inject',
          '-e',
          inputBinary,
          '-o',
          output1,
          '--sea',
          blob1,
        ])

        // Second injection (re-inject)
        await execCommand(BINJECT, [
          'inject',
          '-e',
          output1,
          '-o',
          output2,
          '--sea',
          blob2,
        ])

        // Extract from second output
        const extracted = path.join(testDir, 'reinject_extracted.blob')
        const extractResult = await execCommand(BINJECT, [
          'extract',
          '-e',
          output2,
          '-o',
          extracted,
          '--sea',
        ])

        expect(extractResult.code).toBe(0)

        // Should match blob2, not blob1
        const extractedContent = await fs.readFile(extracted, 'utf8')
        expect(extractedContent).toBe('SECOND_INJECTION')
        expect(extractedContent).not.toBe('FIRST_INJECTION')
      },
      tolerantTimeout(60_000),
    )
  })
})
