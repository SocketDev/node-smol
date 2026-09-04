/*
 * @file Canonical paths for the repo-tier build/release scripts. Inherits the
 *   fleet paths (1 path, 1 reference) and adds the node-smol-specific build
 *   output locations. The Dockerfile path is deliberately NOT constructed
 *   here: `.config/repo/socket-wheelhouse.json` `docker.prebakes[].dockerfile`
 *   is its single source of truth (the prebake-publish workflow reads the same
 *   field), so `build.mts` resolves it from the loaded config instead of a
 *   second hand-maintained copy.
 */

import path from 'node:path'

import { REPO_ROOT } from '../fleet/paths.mts'

export * from '../fleet/paths.mts'

/**
 * Root of untracked build outputs (fleet rule: build outputs live under
 * `<package-root>/build/`, never git-tracked).
 */
export const BUILD_DIR = path.join(REPO_ROOT, 'build')

/**
 * Default staging directory `release.mts` reads assets from. Empty until the
 * binary build lane exists — `build.mts --target binary` names what is
 * missing.
 */
export const RELEASE_ASSETS_DIR = path.join(BUILD_DIR, 'release')
