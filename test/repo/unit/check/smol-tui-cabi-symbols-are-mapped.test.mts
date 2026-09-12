import { mkdirSync, mkdtempSync, readFileSync, writeFileSync } from 'node:fs'
import os from 'node:os'
import path from 'node:path'
import process from 'node:process'
import { safeDelete } from '@socketsecurity/lib-stable/fs/safe'
import { afterEach, beforeEach, expect, test, vi } from 'vitest'

const state = vi.hoisted(() => ({ repoRoot: '', verify: vi.fn() }))
vi.mock(import('../../../../scripts/repo/paths.mts'), async original => ({
  ...(await original()),
  get REPO_ROOT() {
    return state.repoRoot
  },
}))
vi.mock(
  import('../../../../scripts/repo/check/smol-tui-cabi-symbols-are-mapped/source.mts'),
  async original => ({
    ...(await original()),
    verifyStuieSource: state.verify,
  }),
)
import { main } from '../../../../scripts/repo/check/smol-tui-cabi-symbols-are-mapped.mts'

const revision = '1234567890abcdef1234567890abcdef12345678'
let originalArgv: string[]
let originalExitCode: typeof process.exitCode

beforeEach(() => {
  state.repoRoot = mkdtempSync(path.join(os.tmpdir(), 'cabi-offline-'))
  const inputs = path.join(state.repoRoot, 'packages', 'tui-infra')
  mkdirSync(inputs, { recursive: true })
  writeFileSync(
    path.join(state.repoRoot, '.gitmodules'),
    `# stuie-example sha256:${'a'.repeat(64)}\n[submodule "upstream/stuie"]\n path = upstream/stuie\n url = git@github.com:SocketDev/stuie.git\n ref = ${revision}\n`,
  )
  writeFileSync(
    path.join(inputs, 'cabi-symbols.snapshot.json'),
    JSON.stringify({
      files: {},
      symbols: [
        {
          file: 'crates/stuie-cabi/src/cabi.rs',
          name: 'bufferClear',
          signature: 'pub extern \"C\" fn bufferClear()',
          unsafe: false,
        },
      ],
      stuieCommit: revision,
    }),
  )
  writeFileSync(
    path.join(inputs, 'cabi-symbol-map.json'),
    JSON.stringify({
      pendingCeiling: 0,
      rows: [
        {
          symbol: 'bufferClear',
          status: 'out-of-scope',
          reason: 'The fixture renderer does not expose buffers.',
        },
      ],
    }),
  )
  originalArgv = process.argv
  originalExitCode = process.exitCode
  process.argv = ['node', 'cabi-check']
  process.exitCode = undefined
})

afterEach(async () => {
  vi.unstubAllEnvs()
  vi.clearAllMocks()
  process.argv = originalArgv
  process.exitCode = originalExitCode
  await safeDelete(state.repoRoot)
})

test('ordinary validation needs neither a checkout nor remote verification', async () => {
  vi.stubEnv('STUIE_DIR', '/outside/example-stuie')
  await main()
  expect(process.exitCode).toBeUndefined()
  expect(state.verify).not.toHaveBeenCalled()
})

test('a snapshot from another revision fails offline', async () => {
  const snapshot = path.join(
    state.repoRoot,
    'packages',
    'tui-infra',
    'cabi-symbols.snapshot.json',
  )
  writeFileSync(
    snapshot,
    readFileSync(snapshot, 'utf8').replace(revision, '0'.repeat(40)),
  )
  await main()
  expect(process.exitCode).toBe(1)
  expect(state.verify).not.toHaveBeenCalled()
})

test('failed update verification preserves the committed snapshot', async () => {
  const snapshot = path.join(
    state.repoRoot,
    'packages',
    'tui-infra',
    'cabi-symbols.snapshot.json',
  )
  const original = readFileSync(snapshot, 'utf8')
  const failure = new TypeError('Fixture source verification failed')
  state.verify.mockRejectedValueOnce(failure)
  process.argv.push('--update')
  await expect(main()).rejects.toBe(failure)
  expect(readFileSync(snapshot, 'utf8')).toBe(original)
})
