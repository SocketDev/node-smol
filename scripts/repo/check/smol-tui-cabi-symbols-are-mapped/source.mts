import { readFileSync } from 'node:fs'
import path from 'node:path'

import { safeProcessEnv } from '@socketsecurity/lib-stable/env/rewire'
import { spawnSync } from '@socketsecurity/lib-stable/process/spawn/child'

import { GIT_CONTEXT_VARS } from '../../../../.git-hooks/_shared/git-context-vars.mts'
import {
  repositoryContainsTarget,
  repositoryRealPath,
} from '../../../../.git-hooks/_shared/repo-containment.mts'
import { parseBlocks, resolveAll } from '../../../fleet/gen/gitmodules-hash.mts'
import type { Block } from '../../../fleet/gen/gitmodules-hash.mts'

export function readStuieSourcePin(
  repoRoot: string,
): Block & { path: string; ref: string; headerSha: string } {
  const manifest = path.join(repoRoot, '.gitmodules')
  if (!repositoryContainsTarget(repoRoot, manifest)) {
    throw new TypeError(
      'Source pin is outside the repository. Where: .gitmodules. Saw an external manifest, wanted repository configuration. Fix: restore the contained manifest.',
    )
  }
  const pin = parseBlocks(readFileSync(manifest, 'utf8').split(/\r?\n/)).find(
    block => block.ownerRepo === 'SocketDev/stuie',
  )
  if (
    !pin?.path ||
    !pin.ref ||
    !/^[a-f0-9]{40}$/u.test(pin.ref) ||
    !pin.headerSha ||
    !repositoryContainsTarget(repoRoot, path.resolve(repoRoot, pin.path))
  ) {
    throw new TypeError(
      'Source pin is invalid. Where: .gitmodules stuie entry. Saw missing or external source metadata, wanted a contained path with a commit and checksum. Fix: pin upstream/stuie with the fleet materializer.',
    )
  }
  return pin as Block & { path: string; ref: string; headerSha: string }
}

export function assertStuieSourcePath(
  repoRoot: string,
  target: string,
): string {
  const pin = readStuieSourcePin(repoRoot)
  const source = path.resolve(repoRoot, pin.path)
  const realSource = repositoryRealPath(source)
  if (
    !realSource ||
    !repositoryContainsTarget(repoRoot, target) ||
    repositoryRealPath(target) !== realSource
  ) {
    throw new TypeError(
      'Source checkout is invalid. Where: stuie snapshot input. Saw a different checkout, wanted the pinned repository source. Fix: materialize the declared stuie path.',
    )
  }
  return source
}

export function stuieGitEnvironment(): NodeJS.ProcessEnv {
  const env = { ...safeProcessEnv() }
  for (const variable of GIT_CONTEXT_VARS) {
    delete env[variable]
  }
  return env
}

export async function verifyStuieSource(repoRoot: string): Promise<string> {
  const pin = readStuieSourcePin(repoRoot)
  const source = path.resolve(repoRoot, pin.path)
  const head = spawnSync('git', ['-C', source, 'rev-parse', 'HEAD'], {
    encoding: 'utf8',
    env: stuieGitEnvironment(),
  })
  const dirty = spawnSync(
    'git',
    ['-C', source, 'status', '--porcelain', '--untracked-files=all'],
    { encoding: 'utf8', env: stuieGitEnvironment() },
  )
  if (
    head.status !== 0 ||
    typeof head.stdout !== 'string' ||
    head.stdout.trim() !== pin.ref ||
    dirty.status !== 0 ||
    typeof dirty.stdout !== 'string' ||
    dirty.stdout.trim()
  ) {
    throw new TypeError(
      'Source checkout cannot be verified. Where: pinned stuie source. Saw a missing, modified, or different checkout, wanted clean source at the pinned commit. Fix: materialize the declared revision before updating the snapshot.',
    )
  }
  const { 0: verified } = await resolveAll([pin], repoRoot)
  if (!verified?.computed || verified.computed !== pin.headerSha) {
    throw new TypeError(
      'Source checksum does not match. Where: pinned stuie source. Saw missing or different source bytes, wanted the declared checksum. Fix: verify the source pin before updating the snapshot.',
    )
  }
  return source
}
