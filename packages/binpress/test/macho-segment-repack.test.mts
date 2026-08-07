/**
 * @file Mach-O SMOL segment repacking validation tests
 *   Tests Mach-O-specific SMOL segment replacement logic in smol_repack_lief().
 *   These tests validate that:
 *
 *   1. SMOL segments are REPLACED (not appended) during repack
 *   2. SMOL segment structure remains valid after repacking
 *   3. Mach-O binary structure remains valid after repacking
 *   4. Edge cases are handled gracefully IMPORTANT: These tests only run on macOS
 *      platforms where Mach-O is native. They explicitly validate the SMOL
 *      segment handling that mirrors the PT_NOTE handling for ELF binaries.
 */

import crypto from 'node:crypto'
import { existsSync, promises as fs } from 'node:fs'
import os from 'node:os'
import path from 'node:path'
import process from 'node:process'

import { fileURLToPath } from 'node:url'

import { makeExecutable } from 'node-smol-packages-build-infra/lib/build-helpers'
import { getBuildMode } from 'node-smol-packages-build-infra/lib/constants'

import { safeDelete, safeMkdir } from '@socketsecurity/lib-stable/fs/safe'

import {
  MACHO_HEADER_OFFSET,
  MACHO_HEADER_SIZE,
  MACHO_LC_SEGMENT_64_OFFSET,
  MACHO_LC_SEGMENT_OFFSET,
  MACHO_LOAD_COMMAND,
  MACHO_MAGIC,
} from 'node-smol-packages-bin-infra/test/helpers/binary-format-constants'
import {
  codeSignBinary,
  execCommand,
} from 'node-smol-packages-bin-infra/test/helpers/test-utils'

import { afterAll, beforeAll, describe, expect, it } from 'vitest'
import { tolerantTimeout } from '../../../test/fleet/_shared/lib/timing.mts'
import {
  BINPRESS,
  BUILD_MODE,
  NODE_BINARY,
  PACKAGE_DIR,
} from './helpers/binpress-env.mts'

const TIMEOUT_60S = tolerantTimeout(60_000)
const TIMEOUT_90S = tolerantTimeout(90_000)

import {
  countSmolSegments,
  findSmolSegments,
  hasMarkerInSmolSegment,
  parseMachoHeader,
} from './helpers/macho-segments.mts'
import type { SmolSegment } from './helpers/macho-segments.mts'
// Only run on macOS where Mach-O is native
describe.skipIf(process.platform !== 'darwin' || !existsSync(BINPRESS))(
  'mach-O SMOL Segment Repacking Validation',
  () => {
    let testDir: string
    let testBinary: string

    beforeAll(async () => {
      // Create unique test directory with timestamp and random suffix to isolate from parallel runs
      const uniqueId = crypto.randomUUID()
      testDir = path.join(os.tmpdir(), `binpress-macho-segment-${uniqueId}`)
      await safeMkdir(testDir)

      // Copy Node.js binary as consistent test input (not BINPRESS which may vary between builds)
      testBinary = path.join(testDir, 'test-node')
      await fs.copyFile(NODE_BINARY, testBinary)
      await makeExecutable(testBinary)
    })

    afterAll(async () => {
      // Clean up test directory
      if (testDir) {
        await safeDelete(testDir)
      }
    })

    it(
      'should replace SMOL segment (not append)',
      async () => {
        // Step 1: Create initial compressed stub from test binary
        const initialStub = path.join(testDir, 'initial-stub')
        const compressResult = await execCommand(BINPRESS, [
          testBinary,
          '-o',
          initialStub,
        ])

        expect(compressResult.code).toBe(0)
        expect(existsSync(initialStub)).toBeTruthy()

        // Parse Mach-O and count SMOL segments
        const initialMachoData = await fs.readFile(initialStub)
        const initialSmolCount = countSmolSegments(initialMachoData)

        // Should have exactly 1 SMOL segment with compressed data
        expect(initialSmolCount).toBe(1)

        // Verify marker is present
        const hasInitialMarker = hasMarkerInSmolSegment(
          initialMachoData,
          '__SMOL_PRESSED_DATA_MAGIC_MARKER',
        )
        expect(hasInitialMarker).toBeTruthy()

        // Step 2: Update stub with new data (automatic repack detection)
        const updatedStub = path.join(testDir, 'updated-stub')
        const updateResult = await execCommand(BINPRESS, [
          initialStub, // Already compressed, will auto-detect and repack
          '-o',
          updatedStub,
        ])

        expect(updateResult.code).toBe(0)
        expect(existsSync(updatedStub)).toBeTruthy()

        // Parse updated Mach-O and verify SMOL is replaced, not appended
        const updatedMachoData = await fs.readFile(updatedStub)
        const updatedSmolCount = countSmolSegments(updatedMachoData)

        // CRITICAL: Should still have same number of SMOL segments
        // (replaced, not appended)
        expect(updatedSmolCount).toBe(initialSmolCount)
        expect(updatedSmolCount).toBe(1)

        // Verify marker is still present
        const hasUpdatedMarker = hasMarkerInSmolSegment(
          updatedMachoData,
          '__SMOL_PRESSED_DATA_MAGIC_MARKER',
        )
        expect(hasUpdatedMarker).toBeTruthy()
      },
      TIMEOUT_60S,
    )

    it(
      'should maintain Mach-O binary validity after repack',
      async () => {
        const initialStub = path.join(testDir, 'validity-stub')
        await execCommand(BINPRESS, [testBinary, '-o', initialStub])

        const updatedStub = path.join(testDir, 'validity-updated')
        const updateResult = await execCommand(BINPRESS, [
          initialStub, // Automatic repack detection
          '-o',
          updatedStub,
        ])

        expect(updateResult.code).toBe(0)

        // Verify Mach-O magic bytes (little-endian)
        const machoData = await fs.readFile(updatedStub)

        const magic = machoData.readUInt32LE(0)
        const MH_MAGIC_64 = 0xfe_ed_fa_cf

        // Should be Mach-O 64-bit (little-endian - we only support little-endian)
        expect(magic).toBe(MH_MAGIC_64)

        // Verify binary is executable
        await makeExecutable(updatedStub)
        await codeSignBinary(updatedStub)

        const execResult = await execCommand(updatedStub, ['--version'])

        expect(execResult.code).toBe(0)
      },
      TIMEOUT_60S,
    )

    it(
      'should have valid SMOL segment structure after repack',
      async () => {
        const initialStub = path.join(testDir, 'structure-stub')
        await execCommand(BINPRESS, [testBinary, '-o', initialStub])

        const updatedStub = path.join(testDir, 'structure-updated')
        await execCommand(BINPRESS, [
          initialStub, // Automatic repack detection
          '-o',
          updatedStub,
        ])

        const machoData = await fs.readFile(updatedStub)

        // Parse and validate SMOL segment structure
        const segments = findSmolSegments(machoData)

        // Should have exactly one SMOL segment
        expect(segments).toHaveLength(1)

        const smolSegment = segments[0]
        if (smolSegment === undefined) {
          throw new Error('expected a SMOL segment in the repacked binary')
        }
        expect(smolSegment.size).toBeGreaterThan(0)

        // Verify marker is at a valid position
        const markerIndex = smolSegment.content.indexOf(
          Buffer.from('__SMOL_PRESSED_DATA_MAGIC_MARKER', 'utf8'),
        )
        expect(markerIndex).toBeGreaterThanOrEqual(0)
      },
      TIMEOUT_60S,
    )

    it(
      'should handle multiple sequential updates',
      async () => {
        // Create initial stub from test binary
        const stub1 = path.join(testDir, 'multi-stub-1')
        await execCommand(BINPRESS, [testBinary, '-o', stub1])

        const machoData1 = await fs.readFile(stub1)
        const smolCount1 = countSmolSegments(machoData1)
        expect(smolCount1).toBe(1)

        // Update 1: Automatic repack
        const stub2 = path.join(testDir, 'multi-stub-2')
        const result2 = await execCommand(BINPRESS, [
          stub1, // Automatic repack detection
          '-o',
          stub2,
        ])
        expect(result2.code).toBe(0)

        const machoData2 = await fs.readFile(stub2)
        const smolCount2 = countSmolSegments(machoData2)
        // Should not increase
        expect(smolCount2).toBe(smolCount1)
        expect(smolCount2).toBe(1)

        // Update 2: Automatic repack again
        const stub3 = path.join(testDir, 'multi-stub-3')
        const result3 = await execCommand(BINPRESS, [
          stub2, // Automatic repack detection
          '-o',
          stub3,
        ])
        expect(result3.code).toBe(0)

        const machoData3 = await fs.readFile(stub3)
        const smolCount3 = countSmolSegments(machoData3)
        // Should still be the same
        expect(smolCount3).toBe(smolCount1)
        expect(smolCount3).toBe(1)

        // Verify final binary is executable
        await makeExecutable(stub3)
        await codeSignBinary(stub3)

        const execResult = await execCommand(stub3, ['--version'])
        expect(execResult.code).toBe(0)
      },
      TIMEOUT_90S,
    )

    it(
      'should preserve marker after repack',
      async () => {
        const initialStub = path.join(testDir, 'marker-stub')
        await execCommand(BINPRESS, [testBinary, '-o', initialStub])

        const initialData = await fs.readFile(initialStub)
        const hasInitialMarker = hasMarkerInSmolSegment(
          initialData,
          '__SMOL_PRESSED_DATA_MAGIC_MARKER',
        )
        expect(hasInitialMarker).toBeTruthy()

        // Update stub with automatic repack
        const updatedStub = path.join(testDir, 'marker-updated')
        await execCommand(BINPRESS, [
          initialStub, // Automatic repack detection
          '-o',
          updatedStub,
        ])

        const updatedData = await fs.readFile(updatedStub)
        const hasUpdatedMarker = hasMarkerInSmolSegment(
          updatedData,
          '__SMOL_PRESSED_DATA_MAGIC_MARKER',
        )
        expect(hasUpdatedMarker).toBeTruthy()
      },
      TIMEOUT_60S,
    )

    it(
      'should create initial compressed binary',
      async () => {
        // Test initial compression (not update mode)
        const outputStub = path.join(testDir, 'new-compressed-stub')

        const result = await execCommand(BINPRESS, [
          testBinary,
          '-o',
          outputStub,
        ])

        // Should succeed
        expect(result.code).toBe(0)

        // Verify output has SMOL segment with marker
        const outputData = await fs.readFile(outputStub)
        const hasMarker = hasMarkerInSmolSegment(
          outputData,
          '__SMOL_PRESSED_DATA_MAGIC_MARKER',
        )
        expect(hasMarker).toBeTruthy()
      },
      TIMEOUT_60S,
    )

    it(
      'should maintain correct load command count',
      async () => {
        const initialStub = path.join(testDir, 'loadcmd-stub')
        await execCommand(BINPRESS, [testBinary, '-o', initialStub])

        const initialData = await fs.readFile(initialStub)
        const initialHeader = parseMachoHeader(initialData)
        const initialNcmds = initialHeader.ncmds

        const updatedStub = path.join(testDir, 'loadcmd-updated')
        await execCommand(BINPRESS, [
          initialStub, // Automatic repack detection
          '-o',
          updatedStub,
        ])

        const updatedData = await fs.readFile(updatedStub)
        const updatedHeader = parseMachoHeader(updatedData)
        const updatedNcmds = updatedHeader.ncmds

        // Load command count should remain the same
        // (SMOL segment replaced, not appended)
        expect(updatedNcmds).toBe(initialNcmds)
      },
      TIMEOUT_60S,
    )
  },
)
