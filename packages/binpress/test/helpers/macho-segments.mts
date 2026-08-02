/**
 * @file Mach-O parsing helpers for the binpress repack suites: locating the SMOL
 *   segments binpress writes into a Mach-O image, and reading its header.
 *   These were exported from the repack spec itself, which made a test file
 *   the home of reusable parsing logic and pushed it past the file-size cap.
 */

import { SMOL_PRESSED_DATA_MAGIC_MARKER } from 'build-infra/lib/constants'

export interface SmolSegment {
  content: Buffer
  offset: number
  size: number
}

/**
 * Count SMOL segments in Mach-O binary.
 *
 * @param {Buffer} machoData - Mach-O binary data.
 *
 * @returns {number} Number of SMOL segments
 */
export function countSmolSegments(machoData: Buffer) {
  const header = parseMachoHeader(machoData)
  const { headerSize, is64bit, ncmds } = header

  let offset = headerSize
  let smolCount = 0

  for (let i = 0; i < ncmds; i++) {
    const cmd = machoData.readUInt32LE(offset)
    const cmdsize = machoData.readUInt32LE(offset + 4)

    const isSegmentCommand = is64bit
      ? cmd === MACHO_LOAD_COMMAND.LC_SEGMENT_64
      : cmd === MACHO_LOAD_COMMAND.LC_SEGMENT

    if (isSegmentCommand) {
      const segmentNameOffset = is64bit
        ? MACHO_LC_SEGMENT_64_OFFSET.SEGNAME
        : MACHO_LC_SEGMENT_OFFSET.SEGNAME

      const segmentName = machoData
        .subarray(offset + segmentNameOffset, offset + segmentNameOffset + 16)
        .toString('utf8')
        .replace(/\0.*$/, '')

      if (segmentName === 'SMOL') {
        smolCount++
      }
    }

    offset += cmdsize
  }

  return smolCount
}

/**
 * Find SMOL segments with their content.
 *
 * @param {Buffer} machoData - Mach-O binary data.
 *
 * @returns {Array} Array of SMOL segment info
 */
export function findSmolSegments(machoData: Buffer): SmolSegment[] {
  const header = parseMachoHeader(machoData)
  const { headerSize, is64bit, ncmds } = header

  let offset = headerSize
  const segments: SmolSegment[] = []

  for (let i = 0; i < ncmds; i++) {
    const cmd = machoData.readUInt32LE(offset)
    const cmdsize = machoData.readUInt32LE(offset + 4)

    const isSegmentCommand = is64bit
      ? cmd === MACHO_LOAD_COMMAND.LC_SEGMENT_64
      : cmd === MACHO_LOAD_COMMAND.LC_SEGMENT

    if (isSegmentCommand) {
      const segmentNameOffset = is64bit
        ? MACHO_LC_SEGMENT_64_OFFSET.SEGNAME
        : MACHO_LC_SEGMENT_OFFSET.SEGNAME

      const segmentName = machoData
        .subarray(offset + segmentNameOffset, offset + segmentNameOffset + 16)
        .toString('utf8')
        .replace(/\0.*$/, '')

      if (segmentName === 'SMOL') {
        let fileoff: number
        let filesize: number

        if (is64bit) {
          // Use constants for correct offsets
          fileoff = Number(
            machoData.readBigUInt64LE(
              offset + MACHO_LC_SEGMENT_64_OFFSET.FILEOFF,
            ),
          )
          filesize = Number(
            machoData.readBigUInt64LE(
              offset + MACHO_LC_SEGMENT_64_OFFSET.FILESIZE,
            ),
          )
        } else {
          fileoff = machoData.readUInt32LE(
            offset + MACHO_LC_SEGMENT_OFFSET.FILEOFF,
          )
          filesize = machoData.readUInt32LE(
            offset + MACHO_LC_SEGMENT_OFFSET.FILESIZE,
          )
        }

        segments.push({
          content: machoData.subarray(fileoff, fileoff + filesize),
          offset: fileoff,
          size: filesize,
        })
      }
    }

    offset += cmdsize
  }

  return segments
}

/**
 * Search for magic marker in SMOL segments.
 *
 * @param {Buffer} machoData - Mach-O binary data.
 * @param {string} marker - Marker string to search for.
 *
 * @returns {boolean} True if marker found
 */
export function hasMarkerInSmolSegment(machoData: Buffer, marker: string) {
  const segments = findSmolSegments(machoData)
  const markerBuffer = Buffer.from(marker, 'utf8')

  for (let i = 0, { length } = segments; i < length; i += 1) {
    const segment = segments[i]
    if (segment === undefined) {
      continue
    }
    if (segment.content.includes(markerBuffer)) {
      return true
    }
  }

  return false
}

/**
 * Parse Mach-O header and return basic information.
 *
 * @param {Buffer} machoData - Mach-O binary data.
 *
 * @returns {Object} Mach-O header information
 *   Note: We only support little-endian binaries.
 */
export function parseMachoHeader(machoData: Buffer) {
  const magic = machoData.readUInt32LE(0)

  const is64bit = magic === MACHO_MAGIC.MH_MAGIC_64

  if (magic !== MACHO_MAGIC.MH_MAGIC_64 && magic !== MACHO_MAGIC.MH_MAGIC) {
    throw new Error(`Invalid Mach-O magic: 0x${magic.toString(16)}`)
  }

  const ncmds = machoData.readUInt32LE(MACHO_HEADER_OFFSET.NCMDS)
  const sizeofcmds = machoData.readUInt32LE(MACHO_HEADER_OFFSET.SIZEOFCMDS)
  const headerSize = is64bit
    ? MACHO_HEADER_SIZE.HEADER_64
    : MACHO_HEADER_SIZE.HEADER_32

  return {
    headerSize,
    is64bit,
    ncmds,
    sizeofcmds,
  }
}
