import { compileFunction, createContext } from 'node:vm'

import { describe, expect, it } from 'vitest'

import { composeScript } from '../scripts/test262/harness.mts'
import type { TestCase } from '../scripts/test262/types.mts'

function runAsyncTest(source: string) {
  const output: string[] = []
  const context = createContext({
    console: {
      log(...values: unknown[]) {
        output.push(values.map(String).join(' '))
      },
    },
  })
  const test: TestCase = {
    attrs: { async: true },
    file: 'test/built-ins/Promise/allKeyed/async-completion.js',
    filePath: 'test/built-ins/Promise/allKeyed/async-completion.js',
    source,
  }

  const script = composeScript(test, 'strict')
  let error: unknown
  try {
    compileFunction(script, [], { parsingContext: context })()
  } catch (caught) {
    error = caught
  }
  return { context, error, output }
}

describe('Test262 async harness', () => {
  it('prints the completion marker without displacing strict mode', () => {
    const result = runAsyncTest('globalThis.testThis = this; $DONE()')

    expect(result.output).toContain('Test262:AsyncTestComplete')
    expect(result.context['testThis']).toBeUndefined()
    expect(result.error).toBeUndefined()
  })

  it('prints the failure marker when an async test fails', () => {
    const result = runAsyncTest('$DONE(new Test262Error("expected failure"))')

    expect(
      result.output.some(line => line.includes('Test262:AsyncTestFailure')),
    ).toBe(true)
  })
})
