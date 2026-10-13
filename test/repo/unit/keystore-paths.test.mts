import path from 'node:path'

import { describe, expect, it } from 'vitest'

import {
  KEYSTORE_INFRA_DIR,
  MONOREPO_ROOT,
} from '../../../packages/node-smol-builder/scripts/paths.mts'

describe('node-smol keystore source path', () => {
  it('resolves to the pinned sockeye source tree', () => {
    expect(KEYSTORE_INFRA_DIR).toBe(
      path.join(
        MONOREPO_ROOT,
        'upstream',
        'sockeye',
        'packages',
        'keystore-infra',
      ),
    )
  })
})
