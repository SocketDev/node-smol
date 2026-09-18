import { afterEach, describe, expect, it, vi } from 'vitest'
import process from 'node:process'
import { main } from '../../../scripts/repo/build.mts'
import {
  NODE_SMOL_BUILD_ENTRY,
  REPO_ROOT,
} from '../../../scripts/repo/paths.mts'
import { runInherit } from '../../../scripts/fleet/registry-infra/shared.mts'

const originalArgv = process.argv
const originalExitCode = process.exitCode

vi.mock(import('../../../scripts/fleet/registry-infra/shared.mts'), () => ({
  runCapture: vi.fn(),
  runInherit: vi.fn().mockResolvedValue(0),
}))

afterEach(() => {
  vi.restoreAllMocks()
  process.argv = originalArgv
  process.exitCode = originalExitCode
})

import { readDescribeManifest } from './util.mts'

describe('repository build entry', () => {
  it('describes itself without starting a build', () => {
    expect(readDescribeManifest('scripts/repo/build.mts')).toMatchObject({
      description: 'Build repository prebake images or node-smol binaries.',
      name: 'build.mts',
    })
  })
})

it('executes the production binary pipeline from the repository root', async () => {
  process.argv = [process.execPath, 'build.mts', '--target', 'binary']
  await main()
  expect(runInherit).toHaveBeenCalledWith(
    process.execPath,
    [NODE_SMOL_BUILD_ENTRY, '--prod', '--yes'],
    REPO_ROOT,
  )
  expect(process.exitCode).toBe(0)
})
