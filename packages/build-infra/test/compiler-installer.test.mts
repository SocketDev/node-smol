import { expect, test } from 'vitest'

import { parseVersion } from '../lib/compiler-installer.mts'

test('bounds semantic version components', () => {
  expect(parseVersion('12.2.0-ubuntu1')).toEqual({
    major: 12,
    minor: 2,
    patch: 0,
  })
  expect(parseVersion(`${'1'.repeat(100_000)}.2.3`)).toBeUndefined()
})
