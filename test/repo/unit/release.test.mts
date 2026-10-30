import { describe, expect, it } from 'vitest'

import { readDescribeManifest } from './util.mts'

describe('repository release entry', () => {
  it('describes itself without creating a release', () => {
    expect(readDescribeManifest('scripts/repo/release.mts')).toMatchObject({
      description: 'Assemble and optionally publish a draft GitHub release.',
      name: 'release.mts',
    })
  })
})
