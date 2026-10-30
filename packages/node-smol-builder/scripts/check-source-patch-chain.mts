#!/usr/bin/env node

import { spawn } from 'node:child_process'
import { mkdtemp, readFile, readdir, rm } from 'node:fs/promises'
import os from 'node:os'
import path from 'node:path'
import process from 'node:process'
import { pipeline } from 'node:stream/promises'
import { fileURLToPath } from 'node:url'

import {
  applyPatch,
  validatePatch,
} from 'build-infra/lib/patch-validator'
import { errorMessage } from 'build-infra/lib/error-utils'
import { spawn as spawnCaptured } from '@socketsecurity/lib-stable/process/spawn/child'
import { getDefaultLogger } from '@socketsecurity/lib-stable/logger/default'

import { readSubmoduleEntry } from 'build-infra/lib/submodule-version'
import { validateNodePatchLayout } from './source-patched/shared/patch-layout.mts'
import { isMainModule } from '../../../scripts/fleet/process/is-main-module.mts'
import { runMain } from '../../../scripts/fleet/process/main/run.mts'
import type { ScriptMeta } from '../../../scripts/fleet/process/main/run.mts'
import type { ScriptResult } from '../../../scripts/fleet/process/script-result.mts'
import { isJsonRequested } from '../../../scripts/fleet/process/script-meta.mts'
import { UPSTREAM_PATH } from './paths.mts'

const logger = getDefaultLogger()
const __dirname = path.dirname(fileURLToPath(import.meta.url))
const PATCHES_DIR = path.resolve(__dirname, '../patches/source-patched')
const SUBMODULE_PATH = 'packages/node-smol-builder/upstream/node'

const SCRIPT_META: ScriptMeta = {
  describe:
    'Apply the complete Node source patch chain to pristine pinned source.',
  heavyJob: 'test',
  help: 'Usage: pnpm run check:source-patch-chain [--json]',
  json: 'result',
}

interface ChildExit {
  code: number | null
  signal: NodeJS.Signals | null
}

function waitForChild(child: ReturnType<typeof spawn>): Promise<ChildExit> {
  return new Promise((resolve, reject) => {
    child.once('error', reject)
    child.once('close', (code, signal) => resolve({ code, signal }))
  })
}

async function exportPinnedSource(
  sourcePath: string,
  targetDir: string,
  sourceCommit: string,
): Promise<void> {
  const archive = spawn(
    'git',
    ['-C', sourcePath, 'archive', '--format=tar', sourceCommit],
    { stdio: ['ignore', 'pipe', 'pipe'] },
  )
  const extractor = spawn('tar', ['-xf', '-', '-C', targetDir], {
    stdio: ['pipe', 'ignore', 'pipe'],
  })
  const archiveExit = waitForChild(archive)
  const extractorExit = waitForChild(extractor)
  const diagnostics: Buffer[] = []
  archive.stderr.on('data', chunk => diagnostics.push(Buffer.from(chunk)))
  extractor.stderr.on('data', chunk => diagnostics.push(Buffer.from(chunk)))

  const exportTask = Promise.all([
    pipeline(archive.stdout, extractor.stdin),
    archiveExit,
    extractorExit,
  ])
  try {
    const [, archiveResult, extractorResult] = await exportTask
    if (archiveResult.code !== 0 || extractorResult.code !== 0) {
      throw new Error(
        `Failed to export pinned Node source archive. ` +
          `git archive exited ${String(archiveResult.code)}; ` +
          `tar exited ${String(extractorResult.code)}.` +
          (diagnostics.length > 0
            ? `\n${Buffer.concat(diagnostics).toString('utf8').trim()}`
            : ''),
      )
    }
  } catch (error) {
    archive.kill()
    extractor.kill()
    await Promise.allSettled([exportTask, archiveExit, extractorExit])
    throw error
  }
}

export async function applyNodeSourcePatchChain(options: {
  patchesDir: string
  sourceCommit: string
  sourcePath: string
}): Promise<number> {
  const { patchesDir, sourceCommit, sourcePath } = {
    __proto__: null,
    ...options,
  }
  const head = await spawnCaptured(
    'git',
    ['-C', sourcePath, 'rev-parse', 'HEAD'],
    {
      encoding: 'utf8',
      stdio: 'pipe',
    },
  )
  const actualCommit = String(head.stdout).trim()
  if (head.code !== 0 || actualCommit !== sourceCommit) {
    throw new Error(
      `Source checkout does not match requested commit. Expected ${sourceCommit}; saw ${actualCommit || `exit ${String(head.code)}`}.`,
    )
  }

  const patchNames = (await readdir(patchesDir))
    .filter(name => name.endsWith('.patch'))
    .toSorted((left, right) => left.localeCompare(right))
  if (patchNames.length === 0) {
    throw new Error(`No static Node patches found in ${patchesDir}`)
  }
  const patches = await Promise.all(
    patchNames.map(async name => {
      const patchPath = path.join(patchesDir, name)
      return {
        content: await readFile(patchPath, 'utf8'),
        name,
        path: patchPath,
      }
    }),
  )
  validateNodePatchLayout(patches)

  const targetDir = await mkdtemp(
    path.join(os.tmpdir(), 'node-source-patch-chain-'),
  )
  let appliedPatchCount = 0
  try {
    await exportPinnedSource(sourcePath, targetDir, sourceCommit)
    for (const { name, path: patchPath } of patches) {
      if (!(await validatePatch(patchPath, targetDir))) {
        throw new Error(
          `Node patch ${name} does not apply independently to pristine source; see patch diagnostics above.`,
        )
      }
    }
    for (const { name, path: patchPath } of patches) {
      if (!(await validatePatch(patchPath, targetDir))) {
        throw new Error(
          `Cumulative Node patch chain rejected ${name}; see patch diagnostics above.`,
        )
      }
      try {
        await applyPatch(patchPath, targetDir)
      } catch (error) {
        throw new Error(
          `Cumulative Node patch chain failed applying ${name}: ${errorMessage(error)}`,
        )
      }
      appliedPatchCount += 1
    }
  } finally {
    await rm(targetDir, { force: true, recursive: true })
  }
  return appliedPatchCount
}

async function main(): Promise<ScriptResult> {
  const pin = readSubmoduleEntry(SUBMODULE_PATH)
  if (!pin.ref) {
    throw new Error(
      `Node submodule has no pinned commit in .gitmodules: ${SUBMODULE_PATH}`,
    )
  }

  const appliedPatchCount = await applyNodeSourcePatchChain({
    patchesDir: PATCHES_DIR,
    sourceCommit: pin.ref,
    sourcePath: UPSTREAM_PATH,
  })

  if (!isJsonRequested(process.argv.slice(2))) {
    logger.success(`Applied ${appliedPatchCount} Node patches to ${pin.ref}.`)
  }
  return { data: { appliedPatchCount, sourceCommit: pin.ref } }
}

if (isMainModule(import.meta.url)) {
  runMain(main, SCRIPT_META)
}
