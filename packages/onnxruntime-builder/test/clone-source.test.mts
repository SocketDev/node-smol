import { mkdirSync, mkdtempSync, writeFileSync } from 'node:fs'
import os from 'node:os'
import path from 'node:path'

import { afterEach, describe, expect, it } from 'vitest'

import { safeDelete } from '@socketsecurity/lib-stable/fs/safe'

import { cloneOnnxSource } from '../scripts/source-cloned/shared/clone-source.mts'

const temporaryDirectories: string[] = []

afterEach(async () => {
  const results = await Promise.allSettled(
    temporaryDirectories.splice(0).map(safeDelete),
  )
  const failure = results.find(result => result.status === 'rejected')
  if (failure) {
    throw failure.reason
  }
})

describe('ONNX source cloning', () => {
  it('uses the supplied config when an existing source is valid', async () => {
    const sharedBuildDir = mkdtempSync(
      path.join(os.tmpdir(), 'node-smol-onnx-source-'),
    )
    temporaryDirectories.push(sharedBuildDir)
    const sharedSourceDir = path.join(sharedBuildDir, 'source')
    const sharedCmakeDepsFile = path.join(sharedSourceDir, 'cmake', 'deps.txt')
    const sharedCmakeWebassemblyFile = path.join(
      sharedSourceDir,
      'cmake',
      'onnxruntime_webassembly.cmake',
    )
    const sharedPostBuildSourceFile = path.join(
      sharedSourceDir,
      'js',
      'web',
      'script',
      'wasm_post_build.js',
    )
    mkdirSync(path.dirname(sharedPostBuildSourceFile), { recursive: true })
    mkdirSync(path.dirname(sharedCmakeDepsFile), { recursive: true })
    const eigenSha1 = '1111111111111111111111111111111111111111'
    writeFileSync(sharedCmakeDepsFile, eigenSha1)
    writeFileSync(
      sharedCmakeWebassemblyFile,
      '# add_compile_definitions(\n  #   BUILD_MLAS_NO_ONNXRUNTIME',
    )
    writeFileSync(sharedPostBuildSourceFile, 'if (matches.length === 0) {')

    const result = await cloneOnnxSource({
      eigenCommit: 'example-eigen-commit',
      eigenSha1,
      onnxRepo: 'https://example.test/onnx.git',
      onnxSha: '2222222222222222222222222222222222222222',
      onnxVersion: 'v1.2.3',
      sharedBuildDir,
      sharedCmakeDepsFile,
      sharedCmakeWebassemblyFile,
      sharedPostBuildSourceFile,
      sharedSourceDir,
    })

    expect(result.artifactPath).toBe(sharedSourceDir)
  })
})
