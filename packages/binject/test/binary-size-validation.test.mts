import { afterAll, beforeAll, describe, expect, it } from 'vitest'
/**
 * @file Binary SIZE validation tests for binject Validates that binaries
 *   maintain valid format structure after injection:
 *
 *   - Mach-O magic numbers and headers remain valid
 *   - ELF magic numbers and headers remain valid
 *   - PE magic numbers and headers remain valid
 *   - Section/segment offsets are correctly updated
 *   - Binary remains executable after modification
 *   - No corruption of existing sections/segments These tests ensure binject
 *     doesn't produce corrupted binaries that would fail to load or execute on
 *     the target platform.
 */

import { existsSync, promises as fs } from 'node:fs'
import os from 'node:os'
import path from 'node:path'
import process from 'node:process'

import { makeExecutable } from 'build-infra/lib/build-helpers'

import { safeDelete, safeMkdir } from '@socketsecurity/lib-stable/fs/safe'

import { getBinjectPath } from './helpers/paths.mts'
import { execCommand } from './helpers/exec-command.mts'
import { MACHO_SEGMENT_NODE_SEA } from 'bin-infra/test/helpers/segment-names'
import { tolerantTimeout } from '../../../test/fleet/_shared/lib/timing.mts'

const TIMEOUT_30S = tolerantTimeout(30_000)

const BINJECT = getBinjectPath()

let testDir: string
// Resolved at module scope on purpose: `describe.skipIf` is evaluated when
// the suite is REGISTERED, before any beforeAll runs. Assigning this in a
// hook left it false at that moment, so skipIf(!binjectExists) was always
// skipIf(true) and the whole suite silently never ran.
const binjectExists = existsSync(BINJECT)

beforeAll(async () => {
  if (!binjectExists) {
    return
  }

  testDir = path.join(os.tmpdir(), `binject-format-${Date.now()}`)
  await safeMkdir(testDir)
})

afterAll(async () => {
  if (testDir) {
    await safeDelete(testDir)
  }
})

describe.skipIf(!binjectExists)(
  'binary format validation after injection',
  () => {
    describe('binary size validation', () => {
      it(
        'should produce binary with expected size increase',
        async () => {
          const inputBinary = path.join(testDir, 'size_input')
          await fs.copyFile(BINJECT, inputBinary)

          // oxlint-disable-next-line socket/prefer-exists-sync -- every fs.stat() in this file consumes stats.size to assert input/output binary size deltas after injection.
          const inputStats = await fs.stat(inputBinary)

          const seaBlob = path.join(testDir, 'size_test.blob')
          // 10KB blob
          const blobContent = Buffer.alloc(10_000)
          await fs.writeFile(seaBlob, blobContent)

          const outputBinary = path.join(testDir, 'size_output')

          // Inject
          await execCommand(BINJECT, [
            'inject',
            '-e',
            inputBinary,
            '-o',
            outputBinary,
            '--sea',
            seaBlob,
          ])

          // oxlint-disable-next-line socket/prefer-exists-sync -- every fs.stat() in this file consumes stats.size to assert input/output binary size deltas after injection.
          const outputStats = await fs.stat(outputBinary)

          // Output should be at least blob size larger, which allows for metadata overhead.
          const expectedMinSize = inputStats.size + blobContent.length
          expect(outputStats.size).toBeGreaterThanOrEqual(expectedMinSize)

          // But not excessively larger (< 10% overhead)
          const maxExpectedSize = inputStats.size + blobContent.length * 1.1
          expect(outputStats.size).toBeLessThan(maxExpectedSize)
        },
        TIMEOUT_30S,
      )

      it(
        'should reject empty SEA blob injection',
        async () => {
          const inputBinary = path.join(testDir, 'empty_input')
          await fs.copyFile(BINJECT, inputBinary)

          const seaBlob = path.join(testDir, 'empty.blob')
          // Empty blob
          await fs.writeFile(seaBlob, Buffer.alloc(0))

          const outputBinary = path.join(testDir, 'empty_output')

          // Inject empty blob (should fail with error)
          const injectResult = await execCommand(BINJECT, [
            'inject',
            '-e',
            inputBinary,
            '-o',
            outputBinary,
            '--sea',
            seaBlob,
          ])

          // Empty blobs should be rejected
          expect(injectResult.code).not.toBe(0)
          expect(injectResult.stderr).toBeTruthy()
          expect(injectResult.stderr.toLowerCase()).toMatch(
            /empty|size|invalid/,
          )
        },
        TIMEOUT_30S,
      )
    })
  },
)
