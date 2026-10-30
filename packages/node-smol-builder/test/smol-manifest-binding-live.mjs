/**
 * Live smol_manifest_native binding verification.
 *
 * Runs the sdxgen-bug-regressions + uv-lock fixtures through the
 * actual C++ binding inside the built smol Node binary. Unlike
 * test/smol-manifest-native.test.mts (which is vitest-driven and
 * skipped on stock Node where internalBinding is unavailable), this
 * script invokes node:smol-manifest directly and exits non-zero on
 * mismatch.
 *
 * Run with:
 * build/dev/<platform-arch>/source/out/Release/node\
 * test/smol-manifest-binding-live.mjs
 *
 * Runs inside the built node-smol binary, which is compiled --without-amaro
 * (no TypeScript stripping), so it must stay .mjs. Do not convert to .mts —
 * the binary cannot strip types.
 *
 * Wired into the equivalence-harness gate per step 4 of
 * docs/plans/smol-manifest-native-full.md.
 */

import { readdirSync, readFileSync } from 'node:fs'
import path from 'node:path'
import { fileURLToPath } from 'node:url'

import { parseLockfile } from 'node:smol-manifest'
import { getDefaultLogger } from '@socketsecurity/lib-stable/logger/default'

const logger = getDefaultLogger()

const __dirname = path.dirname(fileURLToPath(import.meta.url))
const FIXTURES_DIR = path.join(__dirname, 'fixtures/sdxgen-bug-regressions')
const UV_FIXTURES_DIR = path.join(__dirname, 'fixtures/uv-lock')

// Fixture registers — keep in sync with smol-manifest-native.test.mts.
// `enabled: false` fixtures are registered but not exercised yet —
// each names the reason in its row.
const FIXTURES = [
  {
    dir: 'fix1-npm-v1-alias',
    input: 'input.json',
    eco: 'npm',
    fmt: 'npm',
    enabled: true,
  },
  {
    dir: 'fix2a-npm-v3-workspace-name',
    input: 'input.json',
    eco: 'npm',
    fmt: 'npm',
    enabled: true,
  },
  {
    dir: 'fix2b-npm-v3-alias-name',
    input: 'input.json',
    eco: 'npm',
    fmt: 'npm',
    enabled: true,
  },
  {
    dir: 'fix3a-pnpm-v9-empty-version',
    input: 'input.yaml',
    eco: 'npm',
    fmt: 'pnpm',
    enabled: true,
  },
  {
    dir: 'fix3b-pnpm-v9-workspace-file-filter',
    input: 'input.yaml',
    eco: 'npm',
    fmt: 'pnpm',
    enabled: true,
  },
  {
    dir: 'fix4-yarn-depsmeta-inversion',
    input: 'input.lock',
    eco: 'npm',
    fmt: 'yarn',
    enabled: true,
  },
  {
    dir: 'fix5-pnpm-v9-isdev-derivation',
    input: 'input.yaml',
    eco: 'npm',
    fmt: 'pnpm',
    enabled: true,
  },
  {
    dir: 'cargo-patch-unused-no-leak',
    input: 'input.toml',
    eco: 'cargo',
    fmt: 'cargo',
    enabled: true,
  },
]

// uv.lock fixtures live in their own tracked dir (fixtures/uv-lock/,
// re-included in the root .gitignore) with *.golden.json reference
// outputs. enabled flips to true on the next smol binary build — a
// binary built before parser_uv.cc landed throws ERR_OUT_OF_RANGE
// for the pypi/uv enum pair.
const UV_FIXTURES = [
  {
    dir: 'canonical-headroom-slice',
    input: 'input.uv.lock',
    golden: 'canonical-headroom-slice.golden.json',
    eco: 'pypi',
    fmt: 'uv',
    enabled: false,
  },
]

// Cross-check on-disk dirs match each table.
function assertRegisterMatchesDisk(dir, table, label) {
  const onDisk = readdirSync(dir, { withFileTypes: true })
    .filter(e => e.isDirectory())
    .map(e => e.name)
    .toSorted()
  const inTable = table.map(f => f.dir).toSorted()
  if (JSON.stringify(onDisk) !== JSON.stringify(inTable)) {
    logger.fail(`FIXTURE-TABLE-MISMATCH (${label})`)
    logger.fail('  on disk:', onDisk)
    logger.fail('  in table:', inTable)
    process.exit(1)
  }
}
assertRegisterMatchesDisk(FIXTURES_DIR, FIXTURES, 'sdxgen-bug-regressions')
assertRegisterMatchesDisk(UV_FIXTURES_DIR, UV_FIXTURES, 'uv-lock')

// Flatten both registers into one run list. sdxgen fixtures predate
// the *.golden.json naming guard and keep their expected.json files;
// new fixture sets carry an explicit `golden` filename.
const RUN_LIST = [
  ...FIXTURES.map(f => ({
    ...f,
    baseDir: FIXTURES_DIR,
    golden: 'expected.json',
  })),
  ...UV_FIXTURES.map(f => ({ ...f, baseDir: UV_FIXTURES_DIR })),
]

let pass = 0
let fail = 0
let skip = 0
const failures = []

for (let i = 0, { length } = RUN_LIST; i < length; i += 1) {
  const fixture = RUN_LIST[i]
  if (!fixture.enabled) {
    logger.log(
      `SKIP  ${fixture.dir} (disabled until the next smol binary build)`,
    )
    skip += 1
    continue
  }
  const content = readFileSync(
    path.join(fixture.baseDir, fixture.dir, fixture.input),
    'utf8',
  )
  const expected = JSON.parse(
    readFileSync(
      path.join(fixture.baseDir, fixture.dir, fixture.golden),
      'utf8',
    ),
  )
  const actual = parseLockfile(content, fixture.eco, fixture.fmt)
  const ja = JSON.parse(JSON.stringify(actual))
  const jaStr = JSON.stringify(ja)
  const jeStr = JSON.stringify(expected)
  if (jaStr === jeStr) {
    logger.log(`PASS  ${fixture.dir}`)
    pass += 1
  } else {
    logger.log(`FAIL  ${fixture.dir}`)
    failures.push({ dir: fixture.dir, actual: jaStr, expected: jeStr })
    fail += 1
  }
}

logger.log('')
logger.log(`${pass} pass, ${fail} fail, ${skip} skip`)

if (fail > 0) {
  logger.log('')
  logger.log('Failure details:')
  for (let i = 0, { length } = failures; i < length; i += 1) {
    const f = failures[i]
    logger.log(`  ${f.dir}`)
    logger.log(`    actual  : ${f.actual}`)
    logger.log(`    expected: ${f.expected}`)
  }
  process.exit(1)
}
process.exit(0)
