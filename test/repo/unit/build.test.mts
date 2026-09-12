import { describe, expect, it } from 'vitest'

import { readDescribeManifest } from './util.mts'

describe('repository build entry', () => {
  it('describes itself without starting a build', () => {
    expect(readDescribeManifest('scripts/repo/build.mts')).toMatchObject({
      description: 'Build the repository prebake images.',
      name: 'build.mts',
    })
  })
})
