/**
 * @file Contract tests for the node-smol builtins consumed by stuie.
 */

import { describe, expect, it } from 'vitest'

import {
  runOnSmolBinary,
  smolBuiltinIsAvailable,
} from '../helpers/smol-builtin.mts'

const hasSmolKeymap = smolBuiltinIsAvailable('smol-keymap')
const hasSmolTui = smolBuiltinIsAvailable('smol-tui')

describe.skipIf(!hasSmolKeymap)('stuie node:smol-keymap contract', () => {
  it('matches single keys and chords through stuie-compatible methods', async () => {
    const result = await runOnSmolBinary(`
      const keymap = require('node:smol-keymap')
      const handle = keymap.createKeymap(JSON.stringify({
        'ctrl+a': 'select-all',
        'ctrl+x ctrl+s': 'save'
      }))
      const values = [
        keymap.matchKey(handle, 'a', 1),
        keymap.matchKey(handle, 'x', 1),
        keymap.matchKey(handle, 's', 1)
      ]
      keymap.resetChord(handle)
      keymap.destroyKeymap(handle)
      process.stdout.write(JSON.stringify(values))
    `)

    expect(result).toMatchObject({ code: 0, stderr: '' })
    expect(JSON.parse(result.stdout)).toEqual(['select-all', null, 'save'])
  })
})

describe.skipIf(!hasSmolTui)('stuie node:smol-tui contract', () => {
  it('detects and parses an SGR mouse press through stuie-compatible methods', async () => {
    const result = await runOnSmolBinary(`
      const tui = require('node:smol-tui')
      const data = Buffer.from('\\u001b[<0;12;7M')
      const handle = tui.createParser()
      const detected = tui.looksLikeMouseSequence(data, 0)
      const parsed = tui.parseMouseOne(handle, data, 0)
      tui.resetParser(handle)
      tui.destroyParser(handle)
      process.stdout.write(JSON.stringify({ detected, parsed }))
    `)

    expect(result).toMatchObject({ code: 0, stderr: '' })
    expect(JSON.parse(result.stdout)).toEqual({
      detected: true,
      parsed: {
        consumed: 10,
        event: {
          alt: false,
          button: 0,
          ctrl: false,
          shift: false,
          type: 0,
          x: 11,
          y: 6,
        },
      },
    })
  })
})
