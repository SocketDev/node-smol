/**
 * @file PE parsing helpers for the binpress repack suites: locating and reading
 *   the `.PRESSED` sections binpress writes into a PE image.
 *   These were exported from the repack spec itself, which made a test file the
 *   home of reusable parsing logic and pushed it past the file-size cap.
 */

import { SMOL_PRESSED_DATA_MAGIC_MARKER } from 'node-smol-packages-build-infra/lib/constants'

// PE section name constants
const PRESSED_DATA_SECTION_NAME = '.PRESSED'
const PRESSED_DATA_MAGIC_MARKER = SMOL_PRESSED_DATA_MAGIC_MARKER

export interface PeSection {
  content: Buffer
  name: string
  pointerToRawData: number
  sizeOfRawData: number
  virtualAddress: number
  virtualSize: number
}

/**
 * Count .PRESSED_DATA sections in PE binary.
 *
 * @param {Buffer} peData - PE binary data.
 *
 * @returns {number} Number of .PRESSED_DATA sections
 */
export function countPressedDataSections(peData: Buffer) {
  const sections = parseSections(peData)
  return sections.filter(s => s.name === PRESSED_DATA_SECTION_NAME).length
}

/**
 * Find .PRESSED_DATA sections with their content.
 *
 * @param {Buffer} peData - PE binary data.
 *
 * @returns {Array} Array of .PRESSED_DATA section info
 */
export function findPressedDataSections(peData: Buffer): PeSection[] {
  const sections = parseSections(peData)
  return sections.filter(s => s.name === PRESSED_DATA_SECTION_NAME)
}

/**
 * Search for magic marker in .pressed_data sections.
 *
 * @param {Buffer} peData - PE binary data.
 * @param {string} marker - Marker string to search for.
 *
 * @returns {boolean} True if marker found
 */
export function hasMarkerInPressedDataSection(peData: Buffer, marker: string) {
  const sections = findPressedDataSections(peData)
  const markerBuffer = Buffer.from(marker, 'utf8')

  for (let i = 0, { length } = sections; i < length; i += 1) {
    const section = sections[i]
    if (section === undefined) {
      continue
    }
    if (section.content.includes(markerBuffer)) {
      return true
    }
  }

  return false
}

/**
 * Parse PE header and return basic information.
 *
 * @param {Buffer} peData - PE binary data.
 *
 * @returns {Object} PE header information
 */
export function parsePeHeader(peData: Buffer) {
  // Validate DOS header magic "MZ"
  if (peData[0] !== 0x4d || peData[1] !== 0x5a) {
    throw new Error('Invalid DOS magic (expected MZ)')
  }

  // Get offset to PE header from DOS header (at offset 0x3c)
  const peOffset = peData.readUInt32LE(0x3c)

  // Validate PE signature "PE\0\0"
  if (
    peData[peOffset] !== 0x50 ||
    peData[peOffset + 1] !== 0x45 ||
    peData[peOffset + 2] !== 0x00 ||
    peData[peOffset + 3] !== 0x00
  ) {
    throw new Error('Invalid PE signature')
  }

  // COFF header starts after PE signature (4 bytes)
  const coffHeaderOffset = peOffset + 4

  // Machine type (2 bytes) - not used but validates structure
  // const machine = peData.readUInt16LE(coffHeaderOffset)

  // Number of sections (2 bytes, at offset +2)
  const numberOfSections = peData.readUInt16LE(coffHeaderOffset + 2)

  // Size of optional header (2 bytes, at offset +16)
  const sizeOfOptionalHeader = peData.readUInt16LE(coffHeaderOffset + 16)

  // Section headers start after: PE signature (4) + COFF header (20) + optional header
  const sectionHeadersOffset = coffHeaderOffset + 20 + sizeOfOptionalHeader

  return {
    coffHeaderOffset,
    numberOfSections,
    peOffset,
    sectionHeadersOffset,
  }
}

/**
 * Parse section headers and return array of section info.
 *
 * @param {Buffer} peData - PE binary data.
 *
 * @returns {Array} Array of section information
 */
export function parseSections(peData: Buffer): PeSection[] {
  const header = parsePeHeader(peData)
  const { numberOfSections, sectionHeadersOffset } = header

  const sections: PeSection[] = []

  // Each section header is 40 bytes
  const SECTION_HEADER_SIZE = 40

  for (let i = 0; i < numberOfSections; i++) {
    const offset = sectionHeadersOffset + i * SECTION_HEADER_SIZE

    // Section name (8 bytes, null-padded)
    const nameBuffer = peData.subarray(offset, offset + 8)
    const name = nameBuffer.toString('utf8').replace(/\0.*$/, '')

    // Virtual size (4 bytes, at offset +8)
    const virtualSize = peData.readUInt32LE(offset + 8)

    // Virtual address (4 bytes, at offset +12)
    const virtualAddress = peData.readUInt32LE(offset + 12)

    // Size of raw data (4 bytes, at offset +16)
    const sizeOfRawData = peData.readUInt32LE(offset + 16)

    // Pointer to raw data (4 bytes, at offset +20)
    const pointerToRawData = peData.readUInt32LE(offset + 20)

    sections.push({
      content:
        sizeOfRawData > 0
          ? peData.subarray(pointerToRawData, pointerToRawData + sizeOfRawData)
          : Buffer.alloc(0),
      name,
      pointerToRawData,
      sizeOfRawData,
      virtualAddress,
      virtualSize,
    })
  }

  return sections
}
