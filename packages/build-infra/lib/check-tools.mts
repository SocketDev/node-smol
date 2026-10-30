/**
 * Shared tool checking utility for build packages.
 */

import { existsSync } from 'node:fs'
import process from 'node:process'

import { whichSync } from '@socketsecurity/lib-stable/exe/path/which'
import { isCI as isCIEnvironment } from '@socketsecurity/lib-stable/env/ci'
import { getDefaultLogger } from '@socketsecurity/lib-stable/logger/default'
import { spawn } from '@socketsecurity/lib-stable/process/spawn/child'

import { errorMessage } from './error-utils.mts'
import { ensureAllToolsInstalled } from './tool-installer.mts'

const logger = getDefaultLogger()

/**
 * Check and optionally install required build tools.
 *
 * @param {object} config - Tool configuration.
 * @param {string} config.packageName - Package name for logging.
 * @param {string[]} config.autoInstallableTools - Tools that can be
 *   auto-installed.
 * @param {{
 *   name: string
 *   cmd: string
 *   args?: string[]
 *   isLibrary?: boolean
 * }[]} config.manualTools
 *   - Tools that must be checked manually
 * @param {object} options - Options.
 * @param {boolean} options.autoInstall - Attempt auto-installation.
 * @param {boolean} options.autoYes - Auto-yes to prompts.
 */
export async function checkTools(
  config: {
    autoInstallableTools: string[]
    manualTools: Array<{
      args?: string[] | undefined
      cmd: string
      filePaths?: string[] | undefined
      isLibrary?: boolean | undefined
      name: string
    }>
    packageName: string
  },
  { autoInstall = true, autoYes = false } = {},
) {
  const { autoInstallableTools, manualTools, packageName } = config

  logger.info(`Checking required build tools for ${packageName}...`)
  logger.error('')

  // Check auto-installable tools
  const result = await ensureAllToolsInstalled(autoInstallableTools, {
    autoInstall,
    autoYes,
  })

  // Report auto-installable tools.
  for (let i = 0, { length } = autoInstallableTools; i < length; i += 1) {
    const tool = autoInstallableTools[i]
    if (result.installed.includes(tool)) {
      logger.success(`${tool} installed automatically`)
    } else if (!result.missing.includes(tool)) {
      logger.success(`${tool} is available`)
    }
  }

  const allManualAvailable = await inspectManualTools(manualTools)

  // Handle missing tools.
  if (!result.allAvailable || !allManualAvailable) {
    logger.fail('Some required tools are missing')
    logger.error('')

    reportMissingAutoTools(result.missing)

    logger.error('')
    logger.info(
      'Re-run without --no-auto-install to attempt automatic installation',
    )
    return false
  }

  logger.success('All required tools are available')
  logger.error('')
  return true
}

export async function inspectManualTools(manualTools) {
  let allAvailable = true
  for (let i = 0, { length } = manualTools; i < length; i += 1) {
    const { args, cmd, filePaths, isLibrary, name } = manualTools[i]
    let availablePath
    let found = false
    if (isLibrary && filePaths?.length) {
      for (
        let p = 0, { length: pathCount } = filePaths;
        p < pathCount;
        p += 1
      ) {
        const filePath = filePaths[p]
        if (existsSync(filePath)) {
          availablePath = filePath
          found = true
          break
        }
      }
    }
    if (isLibrary && !found && args && whichSync(cmd, { nothrow: true })) {
      try {
        // eslint-disable-next-line no-await-in-loop
        const result = await spawn(cmd, args, { stdio: 'pipe' })
        found = result.code === 0
      } catch {}
    } else if (!isLibrary) {
      found = Boolean(whichSync(cmd, { nothrow: true }))
    }
    if (found) {
      logger.success(
        `${name} is available${availablePath ? ` (${availablePath})` : ''}`,
      )
    } else {
      logger.fail(`${name} is NOT available`)
      allAvailable = false
    }
  }
  return allAvailable
}

export function reportMissingAutoTools(missingTools) {
  if (missingTools.length === 0) {
    return
  }
  logger.warn('Missing auto-installable tools:')
  for (let i = 0, { length } = missingTools; i < length; i += 1) {
    logger.info(`  - ${missingTools[i]}`)
  }
  if (process.platform === 'darwin') {
    logger.error('')
    logger.info('To install missing tools on macOS:')
    if (missingTools.some(tool => ['clang', 'clang++'].includes(tool))) {
      logger.info('  xcode-select --install')
    }
    for (let i = 0, { length } = missingTools; i < length; i += 1) {
      const tool = missingTools[i]
      if (!['clang', 'clang++'].includes(tool)) {
        logger.info(`  brew install ${tool}`)
      }
    }
  } else if (process.platform === 'linux') {
    logger.error('')
    logger.info('To install missing tools on Linux:')
    logger.info(
      `  sudo apt-get install -y ${missingTools.join(' ')} build-essential`,
    )
  }
}

/**
 * Run a package's check-tools entry-point end-to-end.
 *
 * Wraps the --no-auto-install / --yes CLI parsing + CI auto-yes detection
 * \+ error handling that every packages/<pkg>/scripts/check-tools.mts was
 * hand-rolling.
 *
 * Sets process.exitCode = 1 on failure.
 */
export async function runCheckTools(config: Parameters<typeof checkTools>[0]) {
  try {
    const autoInstall = !process.argv.includes('--no-auto-install')
    const autoYes =
      process.argv.includes('--yes') ||
      isCIEnvironment() ||
      'CONTINUOUS_INTEGRATION' in process.env

    const success = await checkTools(config, { autoInstall, autoYes })
    process.exitCode = success ? 0 : 1
  } catch (e) {
    logger.fail(`Error checking tools: ${errorMessage(e)}`)
    process.exitCode = 1
  }
}
