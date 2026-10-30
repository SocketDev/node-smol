import { describe, expect, it } from 'vitest'

import { mkdirSync, mkdtempSync, rmSync, writeFileSync } from 'node:fs'
import os from 'node:os'
import path from 'node:path'

import { findSocketPatches } from '../../scripts/source-patched/shared/apply-patches.mts'
import { validateNodePatchLayout } from '../../scripts/source-patched/shared/patch-layout.mts'

function makePatch(
  name: string,
  target: string,
): { content: string; name: string } {
  return {
    content: [
      `--- a/${target}`,
      `+++ b/${target}`,
      '@@ -1,1 +1,1 @@',
      '-before',
      '+after',
    ].join('\n'),
    name,
  }
}

describe('Node source patch layout', () => {
  it('accepts unique target files and multiple hunks for one target', () => {
    const first = makePatch('001-first.patch', 'src/first.cc')
    first.content += '\n@@ -8,1 +8,1 @@\n-old value\n+new value'

    expect(() =>
      validateNodePatchLayout([
        first,
        makePatch('002-second.patch', 'src/second.cc'),
      ]),
    ).not.toThrow()
  })

  it('rejects two patches for one target with disjoint hunks', () => {
    const first = makePatch('001-first.patch', 'src/shared.cc')
    const second = makePatch('002-second.patch', 'src/shared.cc')
    second.content = second.content.replace(
      '@@ -1,1 +1,1 @@',
      '@@ -200,1 +200,1 @@',
    )

    expect(() => validateNodePatchLayout([first, second])).toThrow(Error)
  })

  it('rejects a patch with multiple file entries', () => {
    const first = makePatch('001-multi.patch', 'src/first.cc').content
    const second = makePatch('001-multi.patch', 'src/second.cc').content

    expect(() =>
      validateNodePatchLayout([
        { content: `${first}\n${second}`, name: '001-multi.patch' },
      ]),
    ).toThrow(Error)
  })

  it('rejects empty and malformed patches', () => {
    expect(() =>
      validateNodePatchLayout([{ content: '  \n', name: '001-empty.patch' }]),
    ).toThrow(Error)
    expect(() =>
      validateNodePatchLayout([
        { content: '--- a/old.cc\n+++ b/new.cc\n', name: '002-rename.patch' },
      ]),
    ).toThrow(Error)
  })

  it('rejects header timestamps that can alias the same patch target', () => {
    const patch = [
      '--- a/src/file.cc\told timestamp',
      '+++ b/src/file.cc\tnew timestamp',
      '@@ -1,1 +1,1 @@',
      '-before',
      '+after',
    ].join('\n')

    expect(() =>
      validateNodePatchLayout([
        { content: patch, name: '001-timestamp.patch' },
      ]),
    ).toThrow(Error)
  })

  it('rejects duplicate patch names even when targets differ', () => {
    expect(() =>
      validateNodePatchLayout([
        makePatch('001-same.patch', 'src/first.cc'),
        makePatch('001-same.patch', 'src/second.cc'),
      ]),
    ).toThrow(Error)
  })

  it('rejects a dynamic patch that reuses a static patch filename', () => {
    const root = mkdtempSync(path.join(os.tmpdir(), 'node-patch-layout-'))
    const staticDir = path.join(root, 'static')
    const dynamicDir = path.join(root, 'dynamic')
    mkdirSync(staticDir)
    mkdirSync(dynamicDir)
    writeFileSync(
      path.join(staticDir, '001-same.patch'),
      makePatch('001-same.patch', 'src/static.cc').content,
    )
    writeFileSync(
      path.join(dynamicDir, '001-same.patch'),
      makePatch('001-same.patch', 'src/dynamic.cc').content,
    )

    try {
      expect(() => findSocketPatches(staticDir, dynamicDir)).toThrow(Error)
    } finally {
      rmSync(root, { force: true, recursive: true })
    }
  })

  it('does not treat removed text as another file header', () => {
    const patch = [
      '--- a/src/real.cc',
      '+++ b/src/real.cc',
      '@@ -1,1 +0,0 @@',
      '--- a/src/removed-text.cc',
    ].join('\n')

    expect(() =>
      validateNodePatchLayout([{ content: patch, name: '001-text.patch' }]),
    ).not.toThrow()
  })
})
