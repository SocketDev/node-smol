import path from 'node:path'
import process from 'node:process'

import { spawnSync } from '@socketsecurity/lib-stable/process/spawn/child'

export function readDescribeManifest(script: string): Record<string, unknown> {
  const result = spawnSync(
    process.execPath,
    [path.resolve(script), '--describe', '--json'],
    { encoding: 'utf8', stdio: 'pipe' },
  )
  if (result.status !== 0) {
    throw new Error(
      `Describe command exited ${result.status}: ${result.stderr}`,
    )
  }
  return JSON.parse(result.stdout) as Record<string, unknown>
}
