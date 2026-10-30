/**
 * @file Fixtures for the binject resource-size suites: building a file of a
 *   given size, and running a command to completion with its output captured.
 *   Both were exported from the size-limit spec itself, which made a test file
 *   the home of reusable fixtures and pushed it past the file-size cap.
 */

import { promises as fs } from 'node:fs'

import { spawn } from '@socketsecurity/lib-stable/process/spawn/child'

/**
 * Create a file of specified size with pattern data.
 */
export async function createTestFile(filePath: string, sizeBytes: number) {
  // 1MB chunks
  const chunkSize = 1024 * 1024
  const handle = await fs.open(filePath, 'w')

  try {
    let remaining = sizeBytes
    while (remaining > 0) {
      const writeSize = Math.min(chunkSize, remaining)
      // Fill with pattern to avoid compression
      const chunk = Buffer.alloc(writeSize)
      for (let i = 0; i < writeSize; i++) {
        chunk[i] = i % 256
      }
      // eslint-disable-next-line no-await-in-loop
      await handle.write(chunk)
      remaining -= writeSize
    }
  } finally {
    await handle.close()
  }
}

/**
 * Execute command.
 */
interface ExecResult {
  code: number | null
  stderr: string
  stdout: string
}

export async function execCommand(
  command: string,
  args: string[] = [],
  options: Record<string, unknown> = {},
) {
  return new Promise<ExecResult>(resolve => {
    const spawnPromise = spawn(command, args, {
      ...options,
      stdio: ['ignore', 'pipe', 'pipe'],
    })

    // @socketsecurity/lib-stable/process/spawn/child returns a Promise with .process property
    const proc = spawnPromise.process

    let stdout = ''
    let stderr = ''

    proc.stdout?.on('data', data => {
      stdout += data.toString()
    })

    proc.stderr?.on('data', data => {
      stderr += data.toString()
    })

    proc.on('close', code => {
      resolve({ code, stderr, stdout })
    })

    // Handle spawn Promise rejection (non-zero exit codes)
    spawnPromise.catch(() => {
      // Already handled by 'close' event
    })
  })
}
