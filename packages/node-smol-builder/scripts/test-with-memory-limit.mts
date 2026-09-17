#!/usr/bin/env node
/**
 * Cross-platform memory-limited test runner
 * Usage: node scripts/test-with-memory-limit.mts [vitest args...]
 */

import process from 'node:process'

import platformPkg from '@socketsecurity/lib-stable/constants/platform'
import { getDefaultLogger } from '@socketsecurity/lib-stable/logger/default'
import {
  spawn,
  spawnSync,
} from '@socketsecurity/lib-stable/process/spawn/child'
import { errorMessage } from 'local-build-infra/lib/error-utils'

import { getPowerState } from '../../../scripts/fleet/power-state.mts'
import { isMainModule } from '../../../scripts/fleet/process/is-main-module.mts'
import {
  isJsonRequested,
  runMain,
} from '../../../scripts/fleet/process/run-main.mts'
import type { ScriptMeta } from '../../../scripts/fleet/process/run-main.mts'
import type { ScriptResult } from '../../../scripts/fleet/process/script-result.mts'

const logger = getDefaultLogger()
const { WIN32 } = platformPkg

const MAX_MEMORY_MB = 2048 // 2GB limit
const CHECK_INTERVAL_MS = 1000 // Check every 1 second

const SCRIPT_META: ScriptMeta = {
  describe: 'Run Vitest with repository memory and power-aware time limits.',
  help: 'Usage: pnpm run test:e2e [vitest args] [--json]',
  json: 'result',
}

function testErrorResult(error: unknown): ScriptResult {
  const exitCode =
    typeof error === 'object' &&
    error !== null &&
    'code' in error &&
    typeof error.code === 'number'
      ? error.code
      : 1
  return { exitCode, error: errorMessage(error) }
}

function testSpawnResult(
  result: {
    code: number | null
    signal: string | null
    stderr?: unknown | undefined
    stdout?: unknown | undefined
  },
  config: { json: boolean },
): ScriptResult {
  const { json } = { __proto__: null, ...config }
  return {
    exitCode: result.code ?? (result.signal ? 128 : 1),
    data: json
      ? {
          stderr: result.stderr ?? '',
          stdout: result.stdout ?? '',
        }
      : undefined,
  }
}

// Get memory usage cross-platform
export function getMemoryUsageMB(pid) {
  // Handle Ctrl+C
  process.on('SIGINT', () => {
    logger.log('')
    logger.log('Interrupted by user - cleaning up…')
    killed = true
    killProcessTree(vitestProcess.pid)
    clearInterval(monitorInterval)
    process.exitCode = 130
  })

  process.on('SIGTERM', () => {
    killed = true
    killProcessTree(vitestProcess.pid)
    clearInterval(monitorInterval)
    process.exitCode = 143
  })

  try {
    if (WIN32) {
      // Windows: Use PowerShell (wmic is deprecated/removed on Windows 11+)
      const result = spawnSync(
        'powershell.exe',
        ['-NoProfile', '-Command', `(Get-Process -Id ${pid}).WorkingSet64`],
        { encoding: 'utf8' },
      )
      const bytes = parseInt(result.stdout.trim(), 10)
      if (Number.isNaN(bytes)) {
        return 0
      }
      return Math.floor(bytes / 1024 / 1024)
    }
    // macOS/Linux: Use ps
    const result = spawnSync('ps', ['-o', 'rss=', '-p', String(pid)], {
      encoding: 'utf8',
    })
    const kb = parseInt(result.stdout.trim(), 10)
    if (Number.isNaN(kb)) {
      return 0
    }
    return Math.floor(kb / 1024)
  } catch {
    // Process might have exited
    return 0
  }
}

// Kill process tree cross-platform
export function killProcessTree(pid) {
  try {
    if (WIN32) {
      spawnSync('taskkill', ['/pid', String(pid), '/T', '/F'], {
        stdio: 'ignore',
      })
    } else {
      // macOS/Linux: Kill process group
      try {
        process.kill(-pid, 'SIGKILL')
      } catch {
        // Fallback to regular kill
        process.kill(pid, 'SIGKILL')
      }
      // Also kill any child processes
      spawnSync('pkill', ['-9', '-P', String(pid)], { stdio: 'ignore' })
    }
  } catch (e) {
    logger.error(`Error killing process: ${errorMessage(e)}`)
  }
}

export async function main(): Promise<ScriptResult> {
  const ON_AC = (await getPowerState()) === 'ac'
  const json = isJsonRequested(process.argv.slice(2))

  // Test suite builds full SEA binaries (~5s each, ~30 of them) plus
  // VFS extraction tests. On battery, macOS especially throttles CPU
  // hard — local laptop runs are ~2x slower, sometimes ~3x. Use a
  // shorter timeout when plugged in to fail-fast on real regressions
  // and a longer one on battery so a transient power state doesn't
  // kill an otherwise-healthy run.
  const TIMEOUT_MS = ON_AC ? 480_000 : 900_000 // 8 min (AC) | 15 min (battery)

  const vitestArgs = process.argv.slice(2).filter(arg => arg !== '--json')

  if (!json) {
    logger.log(
      `Memory limit: ${MAX_MEMORY_MB}MB | Timeout: ${TIMEOUT_MS / 1000}s ` +
        `(${ON_AC ? 'AC power' : 'battery — extended'})`,
    )
    logger.log(`Running: vitest ${vitestArgs.join(' ')}`)
    logger.log('')
  }

  // Spawn vitest process using @socketsecurity/lib-stable spawn
  const vitestPromise = spawn(
    'pnpm',
    ['exec', 'vitest', 'run', ...vitestArgs],
    {
      shell: WIN32,
      stdio: json ? 'pipe' : 'inherit',
      env: {
        ...process.env,
        NODE_OPTIONS: `--max-old-space-size=${MAX_MEMORY_MB}`,
      },
    },
  )

  const vitestProcess = vitestPromise.process

  const startTime = Date.now()
  let killed = false

  // Memory monitor
  const monitorInterval = setInterval(() => {
    if (killed) {
      clearInterval(monitorInterval)
      return
    }

    const elapsed = Math.floor((Date.now() - startTime) / 1000)

    // Check timeout
    if (Date.now() - startTime > TIMEOUT_MS) {
      logger.error(
        `TIMEOUT: Test exceeded ${TIMEOUT_MS / 1000}s - killing process`,
      )
      killed = true
      killProcessTree(vitestProcess.pid)
      clearInterval(monitorInterval)
      process.exitCode = 124
      return
    }

    // Check memory usage
    const memoryMB = getMemoryUsageMB(vitestProcess.pid)

    if (memoryMB > MAX_MEMORY_MB) {
      logger.error(
        `MEMORY LIMIT EXCEEDED: ${memoryMB}MB > ${MAX_MEMORY_MB}MB - killing process`,
      )
      killed = true
      killProcessTree(vitestProcess.pid)
      clearInterval(monitorInterval)
      process.exitCode = 125
      return
    }

    if (memoryMB > 0) {
      const memPercent = Math.floor((memoryMB / MAX_MEMORY_MB) * 100)
      const bar = '\u2588'.repeat(Math.floor(memPercent / 5))
      const empty = '\u2591'.repeat(20 - Math.floor(memPercent / 5))
      const status = `Memory: ${memoryMB}MB / ${MAX_MEMORY_MB}MB [${bar}${empty}] ${memPercent}% | ${elapsed}s`
      logger.info(status)
    }
  }, CHECK_INTERVAL_MS)

  // Handle vitest exit and errors
  try {
    const result = await vitestPromise
    clearInterval(monitorInterval)
    if (!killed) {
      if (!json) {
        logger.log('')
        logger.log('')
        logger.log(`Test completed with exit code: ${result.code}`)
      }
      return testSpawnResult(result, { json })
    }
  } catch (error) {
    clearInterval(monitorInterval)
    if (!killed) {
      if (!json) {
        logger.error('')
        logger.error(`Test failed: ${errorMessage(error)}`)
      }
      return testErrorResult(error)
    }
  }

  return { exitCode: Number(process.exitCode ?? 1) }
}

if (isMainModule(import.meta.url)) {
  runMain(main, SCRIPT_META)
}
