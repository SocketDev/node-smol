/**
 * @file Preflight checks runner for build scripts.
 *   Provides a DRY way to run common pre-build validation checks.
 */

import { getDefaultLogger } from '@socketsecurity/lib-stable/logger/default'

import {
  checkCompiler,
  checkDiskSpace,
  checkPythonVersion,
} from './build-steps.mts'
import { printError } from './build-output.mts'
import { getMinPythonVersion } from './tool-versions.mts'

const logger = getDefaultLogger()

/**
 * Run preflight checks for build scripts.
 *
 * @param {object} options - Check options.
 * @param {boolean} [options.disk=true] - Check disk space.
 * @param {number} [options.diskGB=5] - Required disk space in GB.
 * @param {boolean} [options.compiler=false] - Check for C++ compiler.
 * @param {string | string[]} [options.compilers] - Specific compiler(s) to
 *   check.
 * @param {boolean} [options.python=false] - Check Python version.
 * @param {string} [options.pythonVersion] - Minimum Python version (defaults to
 *   external-tools.json minimumVersion)
 * @param {boolean} [options.quiet=false] - Suppress output.
 * @param {boolean} [options.failFast=true] - Exit on first failure.
 *
 * @returns {Promise<{ passed: boolean; failures: string[] }>}
 */
export async function runPreflightChecks(options = {}) {
  const {
    compiler = false,
    compilers,
    disk = true,
    diskGB = 5,
    failFast = true,
    python = false,
    pythonVersion,
    quiet = false,
  } = options

  // Use single source of truth from external-tools.json if no version specified
  const effectivePythonVersion = pythonVersion ?? getMinPythonVersion()

  const failures = []

  if (!quiet) {
    logger.step('Running preflight checks')
    logger.log('')
  }

  if (
    await runPreflightStep({
      check: async () => {
        const result = await checkDiskSpace('.', diskGB)
        return result.sufficient
          ? undefined
          : `Insufficient disk space: ${result.availableGB}GB available, ${diskGB}GB required`
      },
      enabled: disk,
      failFast,
      failures,
      quiet,
    })
  ) {
    return { __proto__: null, failures, passed: false }
  }

  if (
    await runPreflightStep({
      check: async () => {
        const result = await checkCompiler(compilers)
        if (result.available) {
          return undefined
        }
        return compilers
          ? `No C++ compiler found (tried: ${Array.isArray(compilers) ? compilers.join(', ') : compilers})`
          : 'No C++ compiler found'
      },
      enabled: compiler,
      failFast,
      failures,
      quiet,
    })
  ) {
    return { __proto__: null, failures, passed: false }
  }

  if (
    await runPreflightStep({
      check: async () => {
        const result = await checkPythonVersion(effectivePythonVersion)
        return result.available
          ? undefined
          : `Python ${effectivePythonVersion}+ not found`
      },
      enabled: python,
      failFast,
      failures,
      quiet,
    })
  ) {
    return { __proto__: null, failures, passed: false }
  }

  if (!quiet) {
    if (!failures.length) {
      logger.success('All preflight checks passed')
      logger.log('')
    } else {
      printError(`${failures.length} preflight check(s) failed`)
      logger.log('')
    }
  }

  return {
    __proto__: null,
    failures,
    passed: !failures.length,
  }
}

/**
 * Run preflight checks and exit on failure.
 *
 * @param {object} options - Check options.
 *
 * @returns {Promise<void>}
 */
export async function runPreflightChecksOrExit(options = {}) {
  const result = await runPreflightChecks(options)

  if (!result.passed) {
    if (!options.quiet) {
      logger.error('Preflight checks failed')
      // oxlint-disable-next-line socket/prefer-cached-for-loop -- iterable is not a bare identifier (could be Map/Set/Generator/expression)
      for (const failure of result.failures) {
        logger.error(`  - ${failure}`)
      }
    }
    throw new Error('Preflight checks failed')
  }
}

export async function runPreflightStep({
  check,
  enabled,
  failFast,
  failures,
  quiet,
}) {
  if (!enabled) {
    return false
  }
  const message = await check()
  if (!message) {
    return false
  }
  failures.push(message)
  if (!quiet) {
    printError(message)
  }
  return failFast
}
