/**
 * C Unit Tests Runner.
 *
 * This Vitest test file builds and runs C unit tests from the test/ directory.
 * The C tests use the minunit-style test framework from bin-infra/test.h.
 */

import { beforeAll, describe, expect, it } from 'vitest'

import { existsSync } from 'node:fs'
import path from 'node:path'
import { fileURLToPath } from 'node:url'

import { getBuildMode } from 'local-build-infra/lib/constants'
import { getCurrentPlatformArch } from 'local-build-infra/lib/platform-mappings'

import { WIN32 } from '@socketsecurity/lib-stable/constants/platform'
import { spawn } from '@socketsecurity/lib-stable/process/spawn/child'
import { isSpawnError } from '@socketsecurity/lib-stable/process/spawn/errors'

import { tolerantTimeout } from '../../../test/fleet/_shared/lib/timing.mts'

const __dirname = path.dirname(fileURLToPath(import.meta.url))
const testDir = __dirname

// Every binary the Makefile's TESTS list builds. Each prints the same minunit
// summary the assertions below grep.
const C_TEST_BINARIES = ['stub_environ_test', 'update_config_test']

// Set by the build hook, read by each test.
let outDir = ''

/**
 * Run a command and hand back everything it printed on stdout and stderr.
 *
 * A C suite with a failing case exits non-zero and lib spawn rejects on that,
 * so the rejection's captured output is returned rather than thrown. The
 * caller's assertions then name the failing case instead of surfacing an
 * opaque spawn error.
 */
async function runCapturingOutput(
  command: string,
  args: readonly string[],
): Promise<string> {
  try {
    const result = await spawn(command, args, {
      cwd: testDir,
      stdio: 'pipe',
    })
    return `${result.stdout ?? ''}\n${result.stderr ?? ''}`
  } catch (e) {
    if (isSpawnError(e)) {
      return `${String(e.stdout)}\n${String(e.stderr)}`
    }
    throw e
  }
}

describe('c Unit Tests', () => {
  beforeAll(async () => {
    const buildMode = getBuildMode()
    const platformArch = await getCurrentPlatformArch()
    outDir = path.join(testDir, 'build', buildMode, platformArch, 'out')

    // Build the C tests using the Makefile. Pass the same BUILD_MODE and
    // PLATFORM_ARCH the workspace uses so the Makefile output lands at
    // test/build/<mode>/<platform-arch>/out/ (covered by **/build/ gitignore).
    const makeEnv = [`BUILD_MODE=${buildMode}`, `PLATFORM_ARCH=${platformArch}`]
    await spawn('make', ['-s', 'clean', ...makeEnv], {
      cwd: testDir,
      stdio: 'pipe',
    })
    await spawn('make', ['-s', 'build', ...makeEnv], {
      cwd: testDir,
      stdio: 'pipe',
    })
  }, tolerantTimeout(180_000))

  it.each(C_TEST_BINARIES)(
    'should build and run %s',
    async binaryName => {
      // Verify the binary was built. gcc on Windows appends .exe to -o.
      const builtName = WIN32 ? `${binaryName}.exe` : binaryName
      const binaryPath = path.join(outDir, builtName)
      expect(existsSync(binaryPath)).toBeTruthy()

      // Run the test binary and capture output
      const testOutput = await runCapturingOutput(binaryPath, [])

      // Verify all tests passed (output should contain "Passed: N" with no failures)
      expect(testOutput).toMatch(/Passed:\s+\d+/)
      expect(testOutput).not.toMatch(/Failed:\s+[1-9]/)
    },
    tolerantTimeout(180_000),
  )
})
