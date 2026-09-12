/**
 * @file Tests for model-build utilities (pure helpers).
 */

import { describe, expect, it } from 'vitest'

import { extractPythonPackages } from '../lib/model-build.mts'

describe('model-build', () => {
  describe(extractPythonPackages, () => {
    it('returns packages acquired from PyPI', () => {
      const packages = extractPythonPackages({
        torch: { origin: 'pypi', version: '2.10.0' },
        transformers: { origin: 'pypi', version: '4.53.3' },
        pnpm: { origin: 'system', version: '11.0.0-rc.0' },
      })
      expect(packages).toStrictEqual(['torch', 'transformers'])
    })

    it('skips the PyPI bootstrap tool', () => {
      const packages = extractPythonPackages({
        pip: { origin: 'pypi', version: '24.3.1' },
        torch: { origin: 'pypi', version: '2.10.0' },
      })
      // pip refuses to uninstall dpkg-owned pip even with
      // --break-system-packages (no RECORD file), so we can't pip-install
      // pip itself. Ship whatever the distro provides.
      expect(packages).toStrictEqual(['torch'])
    })

    it('should annotate onnxruntime with its import name', () => {
      const packages = extractPythonPackages({
        onnxruntime: { origin: 'pypi', version: '1.24.4' },
      })
      expect(packages).toStrictEqual([
        { __proto__: null, importName: 'onnxruntime', name: 'onnxruntime' },
      ])
    })
  })
})
