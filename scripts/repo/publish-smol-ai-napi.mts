/**
 * @file Assemble and stage the @node-smol/ai N-API family. Native build jobs
 *   upload one artifact per target. This script verifies the executable type,
 *   CPU architecture, package name, and shared version before it copies any
 *   addon into an npm package. Native tails are staged first and the public
 *   Prompt API wrapper last. A human must still approve a real npm stage.
 */

import crypto from 'node:crypto'
import {
  copyFileSync,
  existsSync,
  mkdirSync,
  readFileSync,
  statSync,
  writeFileSync,
} from 'node:fs'
import path from 'node:path'
import process from 'node:process'
import { pathToFileURL } from 'node:url'

import { getDefaultLogger } from '@socketsecurity/lib-stable/logger/default'

import { REPO_ROOT } from '../fleet/paths.mts'
import { uploadNpmPackage } from '../fleet/registry-infra/npm/publish-command.mts'
import { isAlreadyPublished } from '../fleet/registry-infra/npm/registry.mts'
import { listStagedPackages } from '../fleet/registry-infra/npm/shared.mts'
import { verifyStagedEntry } from '../fleet/registry-infra/npm/staged.mts'
import { runInherit } from '../fleet/registry-infra/shared.mts'

const logger = getDefaultLogger()

export const SMOL_AI_NAPI_TARGETS = [
  { machine: 0x1_00_00_0c, magic: 'cffaedfe', platform: 'darwin-arm64' },
  { machine: 0x1_00_00_07, magic: 'cffaedfe', platform: 'darwin-x64' },
  { machine: 183, magic: '7f454c46', platform: 'linux-arm64-gnu' },
  { machine: 183, magic: '7f454c46', platform: 'linux-arm64-musl' },
  { machine: 62, magic: '7f454c46', platform: 'linux-x64-gnu' },
  { machine: 62, magic: '7f454c46', platform: 'linux-x64-musl' },
  { machine: 0xaa_64, magic: '4d5a', platform: 'win32-arm64-msvc' },
  { machine: 0x86_64, magic: '4d5a', platform: 'win32-x64-msvc' },
] as const

export interface StagedNapiArtifact {
  readonly packageDir: string
  readonly packageName: string
  readonly platform: string
  readonly sha256: string
  readonly size: number
}

export function packageNameFor(platform: string): string {
  return `@node-smol/ai.node-${platform}`
}

function readManifest(directory: string): {
  name: string
  version: string
} {
  return JSON.parse(readFileSync(path.join(directory, 'package.json'), 'utf8'))
}

export function readMachine(bytes: Buffer, platform: string): number {
  if (platform.startsWith('darwin-')) {
    return bytes.readUInt32LE(4)
  }
  if (platform.startsWith('linux-')) {
    return bytes.readUInt16LE(18)
  }
  const peOffset = bytes.readUInt32LE(0x3c)
  if (bytes.subarray(peOffset, peOffset + 4).toString('hex') !== '50450000') {
    throw new Error(`Windows artifact for ${platform} has no PE header`)
  }
  return bytes.readUInt16LE(peOffset + 4)
}

export function stageNapiArtifacts(
  artifactsDir: string,
  npmRoot = path.join(REPO_ROOT, 'packages', 'npm', '@node-smol'),
): StagedNapiArtifact[] {
  const staged: StagedNapiArtifact[] = []
  for (const target of SMOL_AI_NAPI_TARGETS) {
    const packageName = packageNameFor(target.platform)
    const packageDir = path.join(
      npmRoot,
      packageName.slice('@node-smol/'.length),
    )
    const source = path.join(
      artifactsDir,
      `smol-ai-napi-${target.platform}`,
      'smol_ai.node',
    )
    const destination = path.join(packageDir, 'build', 'smol_ai.node')

    if (!existsSync(source)) {
      throw new Error(
        `Missing N-API artifact for ${target.platform}: ${source}`,
      )
    }
    const size = statSync(source).size
    if (size < 1000) {
      throw new Error(
        `N-API artifact for ${target.platform} is too small: ${size} bytes`,
      )
    }
    const bytes = readFileSync(source)
    if (
      !bytes
        .subarray(0, target.magic.length / 2)
        .toString('hex')
        .startsWith(target.magic)
    ) {
      throw new Error(
        `N-API artifact for ${target.platform} has the wrong file signature`,
      )
    }
    const machine = readMachine(bytes, target.platform)
    if (machine !== target.machine) {
      throw new Error(
        `N-API artifact for ${target.platform} has machine ${machine}; expected ${target.machine}`,
      )
    }

    const manifest = readManifest(packageDir)
    if (manifest.name !== packageName) {
      throw new Error(
        `Package name mismatch for ${target.platform}: ${manifest.name}`,
      )
    }

    mkdirSync(path.dirname(destination), { recursive: true })
    copyFileSync(source, destination)
    staged.push({
      packageDir,
      packageName,
      platform: target.platform,
      sha256: crypto.createHash('sha256').update(bytes).digest('hex'),
      size,
    })
  }
  return staged
}

export function validateReleaseVersion(
  staged: readonly StagedNapiArtifact[],
  config: { allowPlaceholder: boolean; npmRoot?: string | undefined },
): { packageDirs: string[]; packageNames: string[]; version: string } {
  const opts = { __proto__: null, ...config }
  const npmRoot =
    opts.npmRoot ?? path.join(REPO_ROOT, 'packages', 'npm', '@node-smol')
  const wrapperDir = path.join(npmRoot, 'ai')
  const wrapper = readManifest(wrapperDir)
  const packageDirs = staged.map(item => item.packageDir)
  const packageNames = staged.map(item => item.packageName)

  for (const item of staged) {
    const manifest = readManifest(item.packageDir)
    if (manifest.version !== wrapper.version) {
      throw new Error(
        `${manifest.name}@${manifest.version} does not match ${wrapper.name}@${wrapper.version}`,
      )
    }
  }
  if (!opts.allowPlaceholder && wrapper.version === '0.0.0') {
    throw new Error(
      'Refusing to stage placeholder version 0.0.0. Commit the user-selected release version first.',
    )
  }
  return {
    packageDirs: [...packageDirs, wrapperDir],
    packageNames: [...packageNames, wrapper.name],
    version: wrapper.version,
  }
}

function readArg(name: string): string | undefined {
  const index = process.argv.indexOf(name)
  return index === -1 ? undefined : process.argv[index + 1]
}

export function tarballName(name: string, version: string): string {
  return `${name.replace(/^@/u, '').replace('/', '-')}-${version}.tgz`
}

async function main(): Promise<void> {
  const dryRun = process.argv.includes('--dry-run')
  const shouldStage = process.argv.includes('--staged')
  if (dryRun === shouldStage) {
    throw new Error('Pass exactly one of --dry-run or --staged.')
  }
  if (shouldStage && process.env['GITHUB_ACTIONS'] !== 'true') {
    throw new Error('Staged publishing must run in GitHub Actions with OIDC.')
  }

  const artifactsArg = readArg('--artifacts-dir')
  if (!artifactsArg) {
    throw new Error('Pass --artifacts-dir <downloaded workflow artifacts>.')
  }
  const bundleArg = readArg('--bundle-dir')
  if (!bundleArg) {
    throw new Error('Pass --bundle-dir <exact tarball output directory>.')
  }
  const artifactsDir = path.resolve(artifactsArg)
  const bundleDir = path.resolve(bundleArg)
  mkdirSync(bundleDir, { recursive: true })
  const tag = readArg('--tag') ?? 'latest'
  const staged = stageNapiArtifacts(artifactsDir)
  const release = validateReleaseVersion(staged, {
    allowPlaceholder: dryRun,
  })

  for (const artifact of staged) {
    logger.log(
      `${artifact.packageName}: ${artifact.size} bytes, sha256 ${artifact.sha256}`,
    )
  }

  if (shouldStage) {
    for (const name of release.packageNames) {
      if (await isAlreadyPublished(name, release.version)) {
        throw new Error(
          `${name}@${release.version} is already published. Nothing was staged.`,
        )
      }
    }
  }

  const checksums: string[] = []
  for (let index = 0; index < release.packageDirs.length; index += 1) {
    const directory = release.packageDirs[index]!
    const name = release.packageNames[index]!
    const packedName = tarballName(name, release.version)
    const packedPath = path.join(bundleDir, packedName)
    const packCode = await runInherit(
      'pnpm',
      ['pack', '--pack-destination', bundleDir],
      directory,
    )
    if (packCode !== 0 || !existsSync(packedPath)) {
      throw new Error(`${name} pack exited ${packCode}`)
    }
    const packedSha256 = crypto
      .createHash('sha256')
      .update(readFileSync(packedPath))
      .digest('hex')
    checksums.push(`${packedSha256}  ${packedName}`)
    writeFileSync(
      path.join(bundleDir, 'SHA256SUMS'),
      `${checksums.join('\n')}\n`,
    )

    logger.log(
      `${dryRun ? 'Dry-running' : 'Staging'} ${name}@${release.version}`,
    )
    // The upload itself is the fleet's, not ours. uploadNpmPackage owns the
    // argv, decides provenance from the runner AND the repository visibility,
    // and asserts the trusted-publishing auth posture on both sides of the
    // spawn. Everything around it stays local orchestration: which packages go
    // in what order, the shared-version guard, the already-published check, and
    // the staged-tarball verification below.
    //
    // cwd is the package directory, and no manifest here redirects the publish
    // with publishConfig.directory, so the primitive's default manifestPath of
    // <cwd>/package.json already names the manifest being published.
    //
    // The manifests' `publishConfig.provenance: true` is not a competing
    // provenance decision: it is the fleet's source-side floor, required by
    // scripts/fleet/check/publish-config-is-hardened.mts (release tier)
    // because an attestation can never be attached retroactively (see
    // docs/agents.md/fleet/release-tag-escape-hatch.md). It agrees with
    // resolveUploadProvenance() everywhere this family can publish — a real
    // stage runs only in GitHub Actions on this PUBLIC repo — so do not
    // "reconcile" the keys away: that turns the hardening gate red.
    const upload = await uploadNpmPackage({
      cwd: directory,
      dryRun,
      mode: 'staged',
      tag,
    })
    if (upload.code !== 0) {
      throw new Error(`${name} stage publish exited ${upload.code}`)
    }
    // A zero exit is not proof the OIDC exchange worked — pnpm reports the
    // failure and carries on under whatever other credential the environment
    // holds. uploadNpmPackage has already logged which posture refused and why;
    // this is where the run stops instead of staging the rest of the family
    // under the same wrong identity.
    if (!upload.postureOk) {
      throw new Error(
        `${name}@${release.version} failed the publish auth posture check`,
      )
    }
    if (shouldStage) {
      const entries = await listStagedPackages()
      const entry = entries.find(
        candidate =>
          candidate.name === name && candidate.version === release.version,
      )
      if (
        !entry ||
        !(await verifyStagedEntry(entry, {
          packTarball: async () => packedPath,
        }))
      ) {
        throw new Error(
          `${name}@${release.version} failed staged-tarball verification`,
        )
      }
    }
  }

  if (dryRun) {
    logger.success(
      `Dry-run packed all ${release.packageDirs.length} packages in ${bundleDir}.`,
    )
  } else {
    logger.success(
      `Staged ${release.packageDirs.length} packages. Human approval is still required before they become public.`,
    )
  }
}

const invokedDirectly =
  process.argv[1] !== undefined &&
  import.meta.url === pathToFileURL(process.argv[1]).href
if (invokedDirectly) {
  main().catch((error: unknown) => {
    logger.error(error)
    process.exitCode = 1
  })
}
