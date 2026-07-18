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

function requiredPaths(name: string): readonly string[] {
  const contract = UPSTREAM_CONTRACTS.find(value => value.name === name)
  if (!contract) {
    return []
  }
  return [contract.crate, contract.addonCrate, contract.fixture]
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
    const paths = requiredPaths(contract.name)
    for (let j = 0, { length: pathCount } = paths; j < pathCount; j += 1) {
      const relativePath = paths[j]!
      if (!existsSync(path.join(upstreamRoot, relativePath))) {
        errors.push(`${contract.name}: missing required path ${relativePath}`)
      }
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
