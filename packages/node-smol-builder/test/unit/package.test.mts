/**
 * @file Tests for @socketbin/node-smol-builder package structure and
 *   configuration.
 */

import { describe, expect, it } from 'vitest'

import path from 'node:path'

import { readPackageJson } from '@socketsecurity/lib-stable/packages/read'

import { PACKAGE_ROOT as nodeSmolBuilderDir } from '../../scripts/paths.mts'

describe('node-smol package', () => {
  describe('package.json validation', () => {
    it('should have valid package.json metadata', async () => {
      const pkgJson = await readPackageJson(
        path.join(nodeSmolBuilderDir, 'package.json'),
      )

      // Private-package identity law: `local-<own directory name>` at 0.0.0,
      // so the name itself states this package never ships.
      expect(pkgJson.name).toBe('local-node-smol-builder')
      expect(pkgJson.version).toMatch(/^\d+\.\d+\.\d+$/)
      expect(pkgJson.license).toBe('MIT')
      expect(pkgJson.description).toContain('Node.js')
      expect(pkgJson.private).toBeTruthy()
    })

    it('should have build scripts', async () => {
      const pkgJson = await readPackageJson(
        path.join(nodeSmolBuilderDir, 'package.json'),
      )

      expect(pkgJson.scripts).toBeDefined()
      expect(pkgJson.scripts['build']).toBe(
        'node scripts/common/shared/build.mts',
      )
      expect(pkgJson.scripts['build:all']).toBe(
        'node scripts/common/shared/build.mts --all-platforms',
      )
    })
  })

  describe('package publishing', () => {
    it('should not have publishConfig for npm', async () => {
      const pkgJson = await readPackageJson(
        path.join(nodeSmolBuilderDir, 'package.json'),
      )

      // Private package should not configure npm publishing.
      expect(pkgJson.publishConfig).toBeUndefined()
    })
  })
})
