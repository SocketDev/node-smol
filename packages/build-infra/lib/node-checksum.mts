/**
 * Node.js tarball checksum fetch and verification.
 *
 * Fetches SHASUMS256.txt from nodejs.org and cross-checks the hash stored in
 * .gitmodules. Used at build time to ensure the submodule points to an
 * authentic Node.js release.
 */

import { httpText } from '@socketsecurity/lib-stable/http-request'

import { errorMessage } from './error-utils.mts'
import { getSubmoduleChecksum } from './submodule-version.mts'
import { getNodeVersion } from './tool-version-reader.mts'

export async function fetchNodeChecksum(
  version: string,
  options?: { timeout?: number | undefined } | undefined,
): Promise<
  { hash: string; version: string } | { error: string; version: string }
> {
  options = { __proto__: null, ...options } as typeof options
  const versionTag = `v${version}`
  const timeout = options?.timeout ?? 10_000
  const url = `https://nodejs.org/dist/${versionTag}/SHASUMS256.txt`
  const tarballName = `node-${versionTag}.tar.gz`

  let checksums
  try {
    // Force an uncompressed response. nodejs.org serves SHASUMS256.txt with
    // zstd content-encoding, which httpText/fetchChecksumFile does not decode —
    // the parser then sees binary garbage and returns zero entries, so the
    // real `node-vX.Y.Z.tar.gz` line is reported "not found". Requesting
    // `identity` makes the body plain text the GNU-style parser can read.
    checksums = parseShasums(
      await httpText(url, {
        headers: { 'accept-encoding': 'identity' },
        timeout,
      }),
    )
  } catch (e) {
    // oxlint-disable-next-line typescript/no-unsafe-type-assertion -- null-proto idiom: `__proto__: null` keeps the literal prototype-free but changes its inferred type, so the cast is what makes the declared shape hold.
    return {
      __proto__: null,
      version,
      error: `Failed to fetch ${url}: ${errorMessage(e)}`,
    } as unknown as { error: string; version: string }
  }

  const sri = checksums[tarballName]
  if (!sri) {
    // oxlint-disable-next-line typescript/no-unsafe-type-assertion -- null-proto idiom: `__proto__: null` keeps the literal prototype-free but changes its inferred type, so the cast is what makes the declared shape hold.
    return {
      __proto__: null,
      version,
      error: `${tarballName} not found in SHASUMS256.txt`,
    } as unknown as { error: string; version: string }
  }

  // parseShasums yields the file's own lowercase hex, which is the format both
  // callers want: verifyNodeChecksum compares hex, and the update-node skill
  // writes `sha256:<hex>` into .gitmodules. An SRI form is still decoded so a
  // future switch back to an SRI-producing source stays compatible.
  const hash = sri.startsWith('sha256-')
    ? Buffer.from(sri.slice('sha256-'.length), 'base64').toString('hex')
    : sri

  // oxlint-disable-next-line typescript/no-unsafe-type-assertion -- null-proto idiom: `__proto__: null` keeps the literal prototype-free but changes its inferred type, so the cast is what makes the declared shape hold.
  return { __proto__: null, hash, version } as unknown as {
    hash: string
    version: string
  }
}

/**
 * Fetch the SHA-256 checksum for a Node.js source tarball from nodejs.org.
 *
 * Downloads SHASUMS256.txt from the official Node.js distribution and extracts
 * the checksum for `node-vX.Y.Z.tar.gz`. Used by the update-node skill to
 * store the checksum in .gitmodules during version updates.
 *
 * @example
 *   const result = await fetchNodeChecksum('1.2.3')
 *   if ('hash' in result) {
 *     // Write to .gitmodules: # node-1.2.3 sha256:<result.hash>
 *   }
 *
 * @param {string} version - Node.js version without 'v' prefix (e.g., '1.2.3')
 * @param {object} [options]
 * @param {number} [options.timeout=10_000] - Fetch timeout in milliseconds.
 *
 * @returns {Promise<
 *   { hash: string; version: string } | { error: string; version: string }
 * >}
 */
/**
 * Parse a GNU-style `SHASUMS256.txt` into `{ filename: hexDigest }`.
 *
 * `@socketsecurity/lib`'s `fetchChecksumFile` would do this, but it is a
 * build-stubbed export: the published package compiles the implementation out
 * and calling it throws "compiled out of this @socketsecurity/lib build". The
 * format is two fields — digest, then filename, separated by whitespace, with
 * the binary-mode `*` prefix optional — so parsing it here is cheaper than
 * waiting on the export.
 */
export function parseShasums(text: string): Record<string, string> {
  // oxlint-disable-next-line typescript/no-unsafe-type-assertion -- null-proto idiom: `__proto__: null` keeps the map prototype-free but changes its inferred type.
  const out = { __proto__: null } as unknown as Record<string, string>
  const lines = text.split('\n')
  for (let i = 0, { length } = lines; i < length; i += 1) {
    const line = lines[i]!.trim()
    if (!line || line.startsWith('#')) {
      continue
    }
    const match = /^(?<digest>[0-9a-f]{64})\s+\*?(?<name>\S+)$/.exec(line)
    if (match?.groups) {
      out[match.groups['name']!] = match.groups['digest']!
    }
  }
  return out
}

/**
 * Verify Node.js submodule checksum against nodejs.org SHASUMS256.txt.
 *
 * Fetches the official checksum for the Node.js source tarball and compares
 * it against the checksum stored in .gitmodules. This ensures the submodule
 * points to an authentic Node.js release.
 *
 * @example
 *   const result = await verifyNodeChecksum()
 *   if (!result.valid)
 *     throw new Error(
 *       `Checksum mismatch: ${result.expected} !== ${result.actual}`,
 *     )
 *
 * @param {object} [options]
 * @param {string} [options.version] - Node.js version to verify (default: from
 *   .node-version)
 * @param {number} [options.timeout=10_000] - Fetch timeout in milliseconds.
 *
 * @returns {Promise<{
 *   valid: boolean
 *   expected?: string
 *   actual?: string
 *   version: string
 *   error?: string
 * }>}
 */
export async function verifyNodeChecksum(
  options?:
    | {
        version?: string | undefined
        timeout?: number | undefined
      }
    | undefined,
): Promise<{
  valid: boolean
  expected?: string | undefined
  actual?: string | undefined
  version: string
  error?: string | undefined
}> {
  options = { __proto__: null, ...options } as typeof options
  type VerifyResult = {
    valid: boolean
    expected?: string | undefined
    actual?: string | undefined
    version: string
    error?: string | undefined
  }
  const version = options?.version ?? getNodeVersion()

  const stored = getSubmoduleChecksum(
    'packages/node-smol-builder/upstream/node',
    'node',
  )

  if (!stored) {
    // oxlint-disable-next-line typescript/no-unsafe-type-assertion -- null-proto idiom: `__proto__: null` keeps the literal prototype-free but changes its inferred type, so the cast is what makes the declared shape hold.
    return {
      __proto__: null,
      valid: false,
      version,
      error: 'No checksum found in .gitmodules for node submodule',
    } as unknown as VerifyResult
  }

  const result = await fetchNodeChecksum(version, options)
  if ('error' in result) {
    // oxlint-disable-next-line typescript/no-unsafe-type-assertion -- null-proto idiom: `__proto__: null` keeps the literal prototype-free but changes its inferred type, so the cast is what makes the declared shape hold.
    return {
      __proto__: null,
      valid: false,
      version,
      error: result.error,
    } as unknown as VerifyResult
  }

  // oxlint-disable-next-line typescript/no-unsafe-type-assertion -- null-proto idiom: `__proto__: null` keeps the literal prototype-free but changes its inferred type, so the cast is what makes the declared shape hold.
  return {
    __proto__: null,
    valid: stored.hash === result.hash,
    expected: result.hash,
    actual: stored.hash,
    version,
  } as unknown as VerifyResult
}
