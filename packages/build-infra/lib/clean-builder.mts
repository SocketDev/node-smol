/**
 * Shared clean utility for builder packages.
 *
 * Provides a standardized way to clean build artifacts and checkpoints
 * across all builder packages (onnxruntime, yoga-layout, models, etc.).
 *
 * @module clean-builder
 */

import { existsSync } from 'node:fs'
import path from 'node:path'
import { fileURLToPath } from 'node:url'

import { safeDelete } from '@socketsecurity/lib-stable/fs/safe'
import { getDefaultLogger } from '@socketsecurity/lib-stable/logger/default'

import { cleanCheckpoint } from './checkpoint-manager.mts'

const logger = getDefaultLogger()

/**
 * Clean build artifacts for a builder package.
 *
 * @example
 *   // Basic usage (cleans build/ and checkpoints for prod/dev)
 *   await cleanBuilder('onnxruntime-builder')
 *
 * @example
 *   // Custom directories
 *   await cleanBuilder('models', {
 *     cleanDirs: ['build', 'dist'],
 *     packageDir: path.join(__dirname, '..'),
 *   })
 *
 * @example
 *   // No checkpoint cleaning
 *   await cleanBuilder('node-smol-builder', {
 *     checkpointModes: [],
 *   })
 *
 * @param {string} packageName - Display name for logging (e.g.,
 *   'onnxruntime-builder')
 * @param {object} options - Configuration options.
 * @param {string} options.packageDir - Absolute path to package root (default:
 *   caller's parent dir)
 * @param {string[]} options.cleanDirs - Directories to delete relative to
 *   packageDir (default: ['build'])
 * @param {string[]} options.checkpointModes - Modes to clean checkpoints for
 *   (default: ['prod', 'dev'])
 * @param {string} options.buildDir - Build directory name for checkpoints
 *   (default: 'build')
 * @param {object} options.logger - Custom logger instance (default:
 *   getDefaultLogger())
 *
 * @returns {Promise<void>}
 */
export async function cleanBuilder(packageName, options = {}) {
  const {
    buildDir = 'build',
    checkpointModes = ['prod', 'dev'],
    cleanDirs = ['build'],
    logger: output = logger,
  } = options
  const packageDir = resolvePackageDirectory(options.packageDir)

  output.info(`Cleaning ${packageName}…`)
  const cleanedCount =
    (await cleanDirectories(packageDir, cleanDirs, output)) +
    (await cleanCheckpoints(packageDir, buildDir, checkpointModes, output))

  if (cleanedCount === 0) {
    output.info('Nothing to clean')
  } else {
    output.success('Clean complete')
  }
}

export async function cleanCheckpoints(packageDir, buildDir, modes, output) {
  if (modes.length === 0) {
    return 0
  }
  const buildDirPath = path.join(packageDir, buildDir)
  for (let i = 0, { length } = modes; i < length; i += 1) {
    const modeDirPath = path.join(buildDirPath, modes[i])
    await cleanCheckpoint(modeDirPath, '')
  }
  output.success('Cleaned checkpoints')
  return 1
}

export async function cleanDirectories(packageDir, directories, output) {
  let cleanedCount = 0
  for (let i = 0, { length } = directories; i < length; i += 1) {
    const directory = directories[i]
    const directoryPath = path.join(packageDir, directory)
    if (existsSync(directoryPath)) {
      await safeDelete(directoryPath)
      output.success(`Removed ${directory}/`)
      cleanedCount += 1
    }
  }
  return cleanedCount
}

export function resolvePackageDirectory(packageDir) {
  if (packageDir) {
    return packageDir
  }
  const stackLines = new Error().stack?.split(/\r?\n/) ?? []
  const callerUrl =
    stackLines.length > 2 ? stackLines[2].match(/\(([^)]+)\)/)?.[1] : undefined
  if (!callerUrl) {
    throw new Error('packageDir must be provided or auto-detectable')
  }
  const callerPath = fileURLToPath(callerUrl)
  return path.resolve(path.dirname(callerPath), '..')
}
