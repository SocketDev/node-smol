'use strict'

// Loader for the `node:smol-decmpfs` inverse-reader addon. Prefers a locally
// built `./build/Release/smol_decmpfs.node` (from `node-gyp rebuild`), then a
// per-platform prebuild package, mirroring the decmpfs addon loader. The
// underlying reader is a lock-step C++ port of the Rust `decmpfs::addon`.

const { arch, platform } = process

function abiSuffix() {
  if (platform === 'win32') {
    return '-msvc'
  }
  if (platform === 'linux') {
    const report = process.report?.getReport()
    const glibc =
      report && typeof report === 'object'
        ? report.header?.glibcVersionRuntime
        : undefined
    return glibc ? '-gnu' : '-musl'
  }
  return ''
}

const triple = `${platform}-${arch}${abiSuffix()}`
const platformPackage = `@socketbin/smol-decmpfs.node-${triple}`

function load() {
  try {
    return require('./build/Release/smol_decmpfs.node')
  } catch {}
  try {
    return require(platformPackage)
  } catch {}
  throw new Error(
    `smol-decmpfs: no addon binary for ${triple}. Build from source with ` +
      `\`node-gyp rebuild\` in native/smol-decmpfs, or install ${platformPackage}.`,
  )
}

const binding = load()

module.exports = {
  unwrapIfHybrid: binding.unwrapIfHybrid,
  decodePressedData: binding.decodePressedData,
}
