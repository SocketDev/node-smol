/**
 * @file Tests for version-helpers utilities.
 *   Validates .gitmodules version and checksum parsing.
 */

import { afterEach, describe, expect, it } from 'vitest'

import nock from 'nock'

import { readFileSync } from 'node:fs'
import path from 'node:path'

import {
  fetchNodeChecksum,
  getNodeVersion,
  getSubmoduleChecksum,
  getSubmoduleVersion,
  verifyNodeChecksum,
} from '../lib/version-helpers.mts'
import { REPO_ROOT as monorepoRoot } from '../../../scripts/fleet/paths.mts'
import { tolerantTimeout } from '../../../test/fleet/_shared/lib/timing.mts'

// The fleet test setup runs `nock.disableNetConnect()`, so anything reaching
// nodejs.org must be mocked. Serve the real SHASUMS256.txt shape, using the
// digest recorded for this submodule in .gitmodules so the verify path compares
// like for like.
function mockShasums(version: string, digest: string): void {
  nock('https://nodejs.org')
    .get(`/dist/v${version}/SHASUMS256.txt`)
    .reply(
      200,
      [
        `${digest}  node-v${version}.tar.gz`,
        `${'0'.repeat(64)}  node-v${version}-darwin-arm64.tar.gz`,
        '',
      ].join('\n'),
      { 'content-type': 'text/plain' },
    )
}

// The digest .gitmodules records for the pinned Node submodule.
function storedNodeDigest(): string {
  const gitmodules = readFileSync(
    path.join(monorepoRoot, '.gitmodules'),
    'utf8',
  )
  const match = /# node-v?[\d.]+ sha256:(?<digest>[0-9a-f]{64})/.exec(
    gitmodules,
  )
  if (!match?.groups) {
    throw new Error('no node sha256 comment in .gitmodules')
  }
  return match.groups['digest']!
}

afterEach(() => {
  nock.cleanAll()
})

describe('version-helpers', () => {
  describe(getNodeVersion, () => {
    it('should return a valid semver-like version string', () => {
      const version = getNodeVersion()

      expect(version).toBeDefined()
      expect(version).toMatch(/^\d+\.\d+\.\d+$/)
    })

    it('should match .node-version file content', () => {
      const version = getNodeVersion()
      const fileContent = readFileSync(
        path.join(monorepoRoot, '.node-version'),
        'utf8',
      ).trim()

      expect(version).toBe(fileContent)
    })
  })

  describe(getSubmoduleVersion, () => {
    it('should parse node version from .gitmodules', () => {
      const version = getSubmoduleVersion(
        'packages/node-smol-builder/upstream/node',
        'node',
      )

      expect(version).toBeDefined()
      expect(version).toMatch(/^\d+\.\d+\.\d+$/)
    })

    it('should parse lief version from .gitmodules', () => {
      const version = getSubmoduleVersion(
        'packages/lief-builder/upstream/lief',
        'lief',
      )

      expect(version).toBeDefined()
      expect(version).toMatch(/^\d+\.\d+\.\d+$/)
    })

    it('should match .node-version for node submodule', () => {
      const submoduleVersion = getSubmoduleVersion(
        'packages/node-smol-builder/upstream/node',
        'node',
      )
      const nodeVersion = getNodeVersion()

      expect(submoduleVersion).toBe(nodeVersion)
    })

    it('should not include checksum in version string', () => {
      const version = getSubmoduleVersion(
        'packages/node-smol-builder/upstream/node',
        'node',
      )

      expect(version).not.toContain('sha256')
      expect(version).not.toContain(':')
      expect(version).not.toContain(' ')
    })

    it('should throw for non-existent submodule path', () => {
      expect(() =>
        getSubmoduleVersion('packages/nonexistent/upstream/foo', 'foo'),
      ).toThrow('not found in .gitmodules')
    })

    it('should throw for empty package name', () => {
      expect(() =>
        getSubmoduleVersion('packages/node-smol-builder/upstream/node', ''),
      ).toThrow('Package name cannot be empty')
    })
  })

  describe(getSubmoduleChecksum, () => {
    it('should parse checksum for node submodule', () => {
      const checksum = getSubmoduleChecksum(
        'packages/node-smol-builder/upstream/node',
        'node',
      )

      expect(checksum).toBeDefined()
      expect(checksum!.algorithm).toBe('sha256')
      expect(checksum!.hash).toMatch(/^[0-9a-f]{64}$/)
    })

    it('should return undefined for submodules without checksum', () => {
      // `upstream/stuie` tracks a branch and publishes no release tags, so it
      // carries a version comment with no sha256 — the shape this covers.
      const checksum = getSubmoduleChecksum('upstream/stuie', 'stuie')

      expect(checksum).toBeUndefined()
    })

    it('should throw for empty package name', () => {
      expect(() =>
        getSubmoduleChecksum('packages/node-smol-builder/upstream/node', ''),
      ).toThrow('Package name cannot be empty')
    })
  })

  describe(fetchNodeChecksum, () => {
    it(
      'should fetch checksum for current Node.js version',
      async () => {
        const version = getNodeVersion()
        mockShasums(version, storedNodeDigest())
        const result = await fetchNodeChecksum(version, { timeout: 15_000 })

        expect('hash' in result).toBe(true)
        if ('hash' in result) {
          expect(result.hash).toMatch(/^[0-9a-f]{64}$/)
          expect(result.version).toBe(version)
        }
      },
      tolerantTimeout(20_000),
    )

    it(
      'should return error for non-existent version',
      async () => {
        const result = await fetchNodeChecksum('0.0.1', { timeout: 10_000 })

        expect('error' in result).toBe(true)
      },
      tolerantTimeout(15_000),
    )
  })

  describe(verifyNodeChecksum, () => {
    it(
      'should verify checksum against nodejs.org',
      async () => {
        mockShasums(getNodeVersion(), storedNodeDigest())
        const result = await verifyNodeChecksum({ timeout: 15_000 })

        // Should succeed (stored checksum matches upstream)
        expect(result.version).toMatch(/^\d+\.\d+\.\d+$/)
        expect(result.valid).toBe(true)
        expect(result.expected).toMatch(/^[0-9a-f]{64}$/)
        expect(result.actual).toMatch(/^[0-9a-f]{64}$/)
        expect(result.expected).toBe(result.actual)
      },
      tolerantTimeout(20_000),
    )

    it(
      'should return error for invalid version',
      async () => {
        const result = await verifyNodeChecksum({
          version: '0.0.1',
          timeout: 10_000,
        })

        expect(result.valid).toBe(false)
        expect(result.error).toBeDefined()
      },
      tolerantTimeout(15_000),
    )
  })
})
