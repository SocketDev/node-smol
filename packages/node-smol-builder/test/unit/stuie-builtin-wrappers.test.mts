/**
 * @file Userland wrapper contracts for the node-smol builtins consumed by
 *   stuie.
 */

import { readFileSync } from 'node:fs'
import { createContext, Script } from 'node:vm'

import { describe, expect, it, vi } from 'vitest'

interface CommonJsModule {
  exports: Record<string, unknown>
}

function loadBuiltinWrapper(
  relativePath: string,
  bindingName: string,
  binding: Record<string, unknown>,
): Record<string, unknown> {
  const module: CommonJsModule = { exports: {} }
  const context = createContext({
    internalBinding(name: string) {
      if (name !== bindingName) {
        throw new Error(
          `Builtin wrapper requested the wrong binding. Expected ${bindingName}; saw ${name}. Fix the wrapper binding name.`,
        )
      }
      return binding
    },
    module,
    primordials: { ObjectFreeze: Object.freeze },
  })
  const source = readFileSync(new URL(relativePath, import.meta.url), 'utf8')
  new Script(source).runInContext(context)
  return module.exports
}

describe('stuie builtin wrappers', () => {
  it('exposes the keymap methods and modifier packing that stuie consumes', () => {
    const binding = {
      createKeymap: vi.fn(() => 1),
      destroyKeymap: vi.fn(),
      matchKey: vi.fn(() => 'save'),
      resetChord: vi.fn(),
    }
    const keymap = loadBuiltinWrapper(
      '../../additions/source-patched/lib/smol-keymap.js',
      'smol_keymap',
      binding,
    )

    expect(keymap).toMatchObject(binding)
    expect(keymap['getModifierBits']).toBeTypeOf('function')
    const getModifierBits = keymap['getModifierBits'] as (
      modifiers: Record<string, boolean>,
    ) => number
    expect(
      getModifierBits({ alt: true, ctrl: true, meta: true, shift: true }),
    ).toBe(15)
    expect(Object.isFrozen(keymap)).toBe(true)
  })

  it('exposes the mouse parser methods that stuie consumes', () => {
    const parserMethods = {
      createParser: vi.fn(() => 1),
      destroyParser: vi.fn(),
      looksLikeMouseSequence: vi.fn(() => true),
      parseMouseOne: vi.fn(() => ({ consumed: 1, event: undefined })),
      resetParser: vi.fn(),
    }
    const binding = new Proxy(parserMethods, {
      get(target, property: string) {
        if (property in target) {
          return target[property as keyof typeof target]
        }
        if (
          property === 'align' ||
          property === 'constants' ||
          property === 'direction' ||
          property === 'edge' ||
          property === 'flexDirection' ||
          property === 'justify' ||
          property === 'mouseEventType' ||
          property === 'positionType' ||
          property === 'scrollDirection' ||
          property === 'sizes' ||
          property === 'wrap'
        ) {
          return {}
        }
        return vi.fn()
      },
    })
    const tui = loadBuiltinWrapper(
      '../../additions/source-patched/lib/smol-tui.js',
      'smol_tui',
      binding,
    )

    expect(tui).toMatchObject(parserMethods)
    expect(Object.isFrozen(tui)).toBe(true)
  })
})
