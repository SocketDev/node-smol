/**
 * Source invariants for the environment the stubs hand their child process.
 *
 * Each stub setenv()s SMOL_STUB_PATH and SMOL_CACHE_KEY and then launches the
 * extracted node. setenv() grows the environment block and glibc and musl
 * reallocate it, so the envp pointer main() received can end up addressing the
 * block libc walked away from. A child launched with that pointer never sees
 * the SMOL variables, the inner node finds no VFS payload, and the packed
 * binary degrades to bare node.
 *
 * The behavioural half of this regression, `stub_environ_test.c`, exec's a
 * real child. It stays green on macOS whichever pointer the stub uses, because
 * Apple's libc decides on its own whether the old block still aliases the new
 * one. These source checks are the portable half: they hold on every platform
 * and go red the moment a refactor reintroduces the captured pointer.
 */

import { describe, expect, it } from 'vitest'

import { readFile } from 'node:fs/promises'
import path from 'node:path'
import { fileURLToPath } from 'node:url'

const __dirname = path.dirname(fileURLToPath(import.meta.url))
const stubDir = path.join(
  __dirname,
  '..',
  'src',
  'socketsecurity',
  'bin-stub-builder',
)

// The two stubs that exec their child, so they need the live `environ`.
const POSIX_STUBS = ['elf_stub.c', 'macho_stub.c']
// Every stub, including the Windows one, which must pass no captured envp.
const ALL_STUBS = [...POSIX_STUBS, 'pe_stub.c']

// Position of lpEnvironment in the CreateProcessA parameter list, counting
// from zero. NULL there means "inherit the current environment".
const CREATE_PROCESS_ENVIRONMENT_INDEX = 6

/**
 * Blank out C comments and the contents of string and character literals,
 * keeping every other character in place so the offsets and line numbers of
 * the surviving code still match the file.
 *
 * Blanking comments is what exempts the explanatory notes above each exec
 * site: they name the pattern these checks reject, and a raw text search would
 * flag them. Blanking literal contents does the same for text: a `//` inside a
 * URL never starts a comment, and a log line that mentions `execve()` is not
 * an exec call.
 */
function stripCommentsAndLiterals(source: string): string {
  const out: string[] = []
  let index = 0
  while (index < source.length) {
    const char = source[index] as string
    const next = source[index + 1]
    if (char === '/' && next === '*') {
      out.push('  ')
      index += 2
      while (
        index < source.length &&
        !(source[index] === '*' && source[index + 1] === '/')
      ) {
        out.push(source[index] === '\n' ? '\n' : ' ')
        index += 1
      }
      if (index < source.length) {
        out.push('  ')
        index += 2
      }
      continue
    }
    if (char === '/' && next === '/') {
      while (index < source.length && source[index] !== '\n') {
        out.push(' ')
        index += 1
      }
      continue
    }
    if (char === "'" || char === '"') {
      const quote = char
      out.push(quote)
      index += 1
      while (index < source.length) {
        const inner = source[index] as string
        if (inner === '\\') {
          out.push('  ')
          index += 2
          continue
        }
        if (inner === quote) {
          out.push(quote)
          index += 1
          break
        }
        out.push(inner === '\n' ? '\n' : ' ')
        index += 1
      }
      continue
    }
    out.push(char)
    index += 1
  }
  return out.join('')
}

/**
 * The raw argument text of every call to `callee` in `source`.
 *
 * Nested parentheses are tracked, so a call whose arguments contain another
 * call is captured whole.
 */
function findCallArgumentLists(source: string, callee: string): string[] {
  const found: string[] = []
  const pattern = new RegExp(`(^|[^A-Za-z0-9_])${callee}\\s*\\(`, 'g')
  let match = pattern.exec(source)
  while (match !== null) {
    const open = match.index + match[0].length - 1
    let depth = 0
    let index = open
    while (index < source.length) {
      const char = source[index]
      if (char === '(') {
        depth += 1
      } else if (char === ')') {
        depth -= 1
        if (depth === 0) {
          break
        }
      }
      index += 1
    }
    found.push(source.slice(open + 1, index))
    pattern.lastIndex = index
    match = pattern.exec(source)
  }
  return found
}

/**
 * Split an argument list on its top-level commas, ignoring commas nested
 * inside a call or a subscript.
 */
function splitTopLevelArguments(argumentList: string): string[] {
  const parts: string[] = []
  let depth = 0
  let current = ''
  for (const char of argumentList) {
    if (char === '(' || char === '[') {
      depth += 1
    } else if (char === ')' || char === ']') {
      depth -= 1
    }
    if (char === ',' && depth === 0) {
      parts.push(current.trim())
      current = ''
      continue
    }
    current += char
  }
  const last = current.trim()
  if (last !== '') {
    parts.push(last)
  }
  return parts
}

/**
 * Read a stub source with its comments and literal contents blanked out.
 */
async function readStubCode(name: string): Promise<string> {
  return stripCommentsAndLiterals(
    await readFile(path.join(stubDir, name), 'utf8'),
  )
}

describe('stub source scanner', () => {
  it('ignores an exec written inside a comment', () => {
    const sample = [
      '// execve(output_path, argv, envp) drops every variable set here.',
      '/* execve(output_path, argv, envp) */',
      'execve(output_path, argv, environ);',
    ].join('\n')

    expect(
      findCallArgumentLists(stripCommentsAndLiterals(sample), 'execve'),
    ).toEqual(['output_path, argv, environ'])
  })

  it('ignores an exec named inside a string literal', () => {
    const sample = [
      'DEBUG_LOG("Calling execve()... see https://example.test/x");',
      'execve(output_path, argv, environ);',
    ].join('\n')

    expect(
      findCallArgumentLists(stripCommentsAndLiterals(sample), 'execve'),
    ).toEqual(['output_path, argv, environ'])
  })

  it('flags a call that passes the captured envp', () => {
    const sample = 'execve(output_path, argv, envp);'

    const passesEnvp = findCallArgumentLists(
      stripCommentsAndLiterals(sample),
      'execve',
    ).some(args => /\benvp\b/.test(args))

    expect(passesEnvp).toBe(true)
  })
})

describe('stub environ invariants', () => {
  it.each(ALL_STUBS)(
    '%s never passes a captured envp to execve',
    async name => {
      const source = await readStubCode(name)

      for (const args of findCallArgumentLists(source, 'execve')) {
        expect(args).not.toMatch(/\benvp\b/)
      }
    },
  )

  it.each(POSIX_STUBS)('%s declares the live environ', async name => {
    const source = await readStubCode(name)

    expect(source).toMatch(/^\s*extern\s+char\s*\*\s*\*\s*environ\s*;/m)
  })

  it.each(POSIX_STUBS)(
    '%s execs with environ at every call site',
    async name => {
      const source = await readStubCode(name)

      const calls = findCallArgumentLists(source, 'execve')
      expect(calls.length).toBeGreaterThan(0)
      for (const args of calls) {
        expect(splitTopLevelArguments(args)[2]).toBe('environ')
      }
    },
  )

  it.each(POSIX_STUBS)('%s takes no envp parameter in main', async name => {
    const source = await readStubCode(name)

    // A stub whose main() never receives envp cannot capture it, so the
    // stale-pointer exec is unwritable rather than merely absent.
    const signatures = findCallArgumentLists(source, 'main')
    expect(signatures).toHaveLength(1)
    expect(signatures[0]).not.toMatch(/\benvp\b/)
  })

  it('pe_stub.c inherits the current environment at every CreateProcessA call', async () => {
    const source = await readStubCode('pe_stub.c')

    const calls = findCallArgumentLists(source, 'CreateProcessA')
    expect(calls.length).toBeGreaterThan(0)
    for (const args of calls) {
      expect(
        splitTopLevelArguments(args)[CREATE_PROCESS_ENVIRONMENT_INDEX],
      ).toBe('NULL')
    }
  })
})
