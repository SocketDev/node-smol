import { describe, expect, it } from 'vitest'

import { readDescribeManifest } from './util.mts'

describe('smol-ai test entry', () => {
  it('describes itself without starting tests', () => {
    expect(
      readDescribeManifest('scripts/repo/run-smol-ai-tests.mts'),
    ).toMatchObject({
      description: 'Run the owned tests for one smol-ai workspace.',
      name: 'run-smol-ai-tests.mts',
    })
  })
})
