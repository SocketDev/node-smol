#!/usr/bin/env node
/** Validate that every pinned Rust upstream still satisfies its node-smol ABI. */

import { existsSync } from 'node:fs'
import { execFileSync } from 'node:child_process'
import path from 'node:path'
import process from 'node:process'

import { UPSTREAM_CONTRACTS } from '../../upstream/contracts.mts'

const repoRoot = path.resolve(import.meta.dirname, '../..')

function gitHead(directory: string): string {
  return execFileSync('git', ['-C', directory, 'rev-parse', 'HEAD'], {
    encoding: 'utf8',
  }).trim()
}

export function collectContractErrors(root = repoRoot): string[] {
  const errors: string[] = []
  for (let i = 0, { length } = UPSTREAM_CONTRACTS; i < length; i += 1) {
    const contract = UPSTREAM_CONTRACTS[i]!
    const upstreamRoot = path.join(root, 'upstream', contract.name)
    if (!existsSync(upstreamRoot)) {
      errors.push(`${contract.name}: missing upstream checkout`)
      continue
    }
    const actualRevision = gitHead(upstreamRoot)
    if (actualRevision !== contract.revision) {
      errors.push(
        `${contract.name}: pinned ${actualRevision}, expected ${contract.revision}`,
      )
    }
    // The crate + addon crate live inside the pinned upstream checkout
    // (submodule-relative). The fixture is node-smol-owned shared substrate —
    // a corpus that cannot live in the pinned submodule without advancing its
    // revision — so it resolves from the repo root instead.
    const upstreamPaths = [contract.crate, contract.addonCrate]
    for (let j = 0, { length: pathCount } = upstreamPaths; j < pathCount; j += 1) {
      const relativePath = upstreamPaths[j]!
      if (!existsSync(path.join(upstreamRoot, relativePath))) {
        errors.push(`${contract.name}: missing required path ${relativePath}`)
      }
    }
    if (contract.fixture && !existsSync(path.join(root, contract.fixture))) {
      errors.push(`${contract.name}: missing fixture ${contract.fixture}`)
    }
  }
  return errors
}

export function main(): void {
  const errors = collectContractErrors()
  if (errors.length === 0) {
    console.log(`upstream contracts: ${UPSTREAM_CONTRACTS.length} verified`)
    return
  }
  for (let i = 0, { length } = errors; i < length; i += 1) {
    console.error(`upstream contracts: ${errors[i]!}`)
  }
  process.exitCode = 1
}

if (import.meta.main) {
  main()
}
