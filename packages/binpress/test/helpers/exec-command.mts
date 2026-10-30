/**
 * @file Command runner for the binpress target-flag suite: spawns a command and
 *   resolves with its exit code plus captured output.
 *   Exported from the spec itself, which made a test file the home of a
 *   reusable fixture and pushed it past the file-size cap.
 */

import { spawn } from '@socketsecurity/lib-stable/process/spawn/child'

import type { SpawnOptions } from '@socketsecurity/lib-stable/process/spawn/types'

let testDir: string
let testBinary: string

export interface ExecCommandResult {
  code: number | null
  stderr: string
  stdout: string
}

/**
 * Execute command and return result.
 */
export async function execCommand(
  command: string,
  args: string[] | readonly string[] = [],
  options: SpawnOptions = {},
): Promise<ExecCommandResult> {
  return new Promise<ExecCommandResult>(resolve => {
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
    // We still resolve with the code/stdout/stderr for test assertions
    spawnPromise.catch(() => {
      // Already handled by 'close' event
    })
  })
}
