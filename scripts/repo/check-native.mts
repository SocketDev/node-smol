import process from 'node:process'
import path from 'node:path'
import { copyFile, mkdir } from 'node:fs/promises'
import { RELEASE_ASSETS_DIR } from './paths.mts'
import { spawn } from '@socketsecurity/lib-stable/process/spawn/child'
import { getDefaultLogger } from '@socketsecurity/lib-stable/logger/default'
import { getCurrentPlatformArch } from '../../packages/build-infra/lib/platform-mappings.mts'
import { getBuildPaths } from '../../packages/node-smol-builder/scripts/paths.mts'
import { isMainModule } from '../fleet/process/is-main-module.mts'
import { runMain } from '../fleet/process/run-main.mts'

const logger = getDefaultLogger()

export const NATIVE_TUI_PROBE = `const assert = require('node:assert/strict');
const tui = process.getBuiltinModule('node:smol-tui');
assert.ok(tui);
assert.equal(typeof tui.setFgRgb, 'function');
assert.equal(tui.setFgRgb(12, 34, 56), '\\x1b[38;2;12;34;56m');
assert.equal(tui.stringWidth('socket'), 6);
process.stdout.write('native tui verified\\n');`

export async function main(): Promise<void> {
  const target = await getCurrentPlatformArch()
  const { outputFinalBinary } = getBuildPaths('prod', process.platform, target)
  const result = await spawn(outputFinalBinary, ['-e', NATIVE_TUI_PROBE], {
    stdio: 'pipe',
    timeout: 60_000,
  })
  if (
    result.code !== 0 ||
    !result.stdout.toString().includes('native tui verified')
  ) {
    throw new Error(
      `Native TUI verification failed at ${outputFinalBinary}. Expected ANSI and width operations. Rebuild node-smol with TUI enabled.`,
    )
  }
  await mkdir(RELEASE_ASSETS_DIR, { recursive: true })
  await copyFile(
    outputFinalBinary,
    path.join(
      RELEASE_ASSETS_DIR,
      `node-${target}${process.platform === 'win32' ? '.exe' : ''}`,
    ),
  )
  logger.log(
    process.argv.includes('--json')
      ? JSON.stringify({ ok: true, target })
      : `Verified native TUI for ${target}.`,
  )
}

if (isMainModule(import.meta.url)) {
  runMain(main, {
    describe: 'Verify native node-smol terminal operations.',
    help: 'Usage: pnpm run check:native [--json]',
    json: 'native',
    heavyJob: 'test',
  })
}
