/**
 * @file ELF parsing helpers for the binpress repack suites: locating the PT_NOTE
 *   segments binpress writes into an ELF image, and reading its header.
 *   These were exported from the repack spec itself, which made a test file
 *   the home of reusable parsing logic and pushed it past the file-size cap.
 */

import { SMOL_PRESSED_DATA_MAGIC_MARKER } from 'build-infra/lib/constants'

// ELF PT_NOTE constants
const PRESSED_DATA_MAGIC_MARKER = SMOL_PRESSED_DATA_MAGIC_MARKER

export interface PTNoteSegment {
  content: Buffer
  offset: number
  size: number
}

export interface ElfHeaderInfo {
  e_phentsize: number
  e_phnum: number
  e_phoff: number
  is64bit: boolean
}

/**
 * Count PT_NOTE segments in ELF binary.
 *
 * @param {Buffer} elfData - ELF binary data.
 *
 * @returns {number} Number of PT_NOTE segments
 */
export function countPTNoteSegments(elfData: Buffer) {
  const header = parseElfHeader(elfData)
  const { e_phentsize, e_phnum, e_phoff } = header

  let noteCount = 0

  for (let i = 0; i < e_phnum; i++) {
    const phOffset = e_phoff + i * e_phentsize

    // Read p_type, the first field in the program header.
    const p_type = elfData.readUInt32LE(phOffset)

    // PT_NOTE = 4
    if (p_type === 4) {
      noteCount++
    }
  }

  return noteCount
}

/**
 * Find PT_NOTE segments with their content.
 *
 * @param {Buffer} elfData - ELF binary data.
 *
 * @returns {Array} Array of PT_NOTE segment info
 */
export function findPTNoteSegments(elfData: Buffer): PTNoteSegment[] {
  const header = parseElfHeader(elfData)
  const { e_phentsize, e_phnum, e_phoff, is64bit } = header

  const notes: PTNoteSegment[] = []

  for (let i = 0; i < e_phnum; i++) {
    const phOffset = e_phoff + i * e_phentsize

    const p_type = elfData.readUInt32LE(phOffset)

    if (p_type === 4) {
      // PT_NOTE
      let p_offset: number
      let p_filesz: number

      if (is64bit) {
        p_offset = Number(elfData.readBigUInt64LE(phOffset + 8))
        p_filesz = Number(elfData.readBigUInt64LE(phOffset + 32))
      } else {
        p_offset = elfData.readUInt32LE(phOffset + 4)
        p_filesz = elfData.readUInt32LE(phOffset + 16)
      }

      notes.push({
        content: elfData.subarray(p_offset, p_offset + p_filesz),
        offset: p_offset,
        size: p_filesz,
      })
    }
  }

  return notes
}

/**
 * Search for magic marker in PT_NOTE segments.
 *
 * @param {Buffer} elfData - ELF binary data.
 * @param {string} marker - Marker string to search for.
 *
 * @returns {boolean} True if marker found
 */
export function hasMarkerInPTNote(elfData: Buffer, marker: string) {
  const notes = findPTNoteSegments(elfData)
  const markerBuffer = Buffer.from(marker, 'utf8')

  for (let i = 0, { length } = notes; i < length; i += 1) {
    const note = notes[i]
    if (note === undefined) {
      continue
    }
    if (note.content.includes(markerBuffer)) {
      return true
    }
  }

  return false
}

/**
 * Parse ELF header and return basic information.
 *
 * @param {Buffer} elfData - ELF binary data.
 *
 * @returns {Object} ELF header information
 */
export function parseElfHeader(elfData: Buffer): ElfHeaderInfo {
  // Validate ELF magic
  // 'E' 'L' 'F'
  if (
    elfData[0] !== 0x7f ||
    elfData[1] !== 0x45 ||
    elfData[2] !== 0x4c ||
    elfData[3] !== 0x46
  ) {
    throw new Error('Invalid ELF magic')
  }

  // 1=32-bit, 2=64-bit
  const ei_class = elfData[4]
  // 1=little-endian, 2=big-endian
  const ei_data = elfData[5]
  const is64bit = ei_class === 2
  const isLittleEndian = ei_data === 1

  if (!isLittleEndian) {
    throw new Error('Big-endian ELF not supported')
  }

  let e_phoff: number
  let e_phentsize: number
  let e_phnum: number

  if (is64bit) {
    // 64-bit ELF header offsets
    // Program header offset
    e_phoff = Number(elfData.readBigUInt64LE(32))
    // Program header entry size
    e_phentsize = elfData.readUInt16LE(54)
    // Number of program headers
    e_phnum = elfData.readUInt16LE(56)
  } else {
    // 32-bit ELF header offsets
    e_phoff = elfData.readUInt32LE(28)
    e_phentsize = elfData.readUInt16LE(42)
    e_phnum = elfData.readUInt16LE(44)
  }

  return {
    e_phentsize,
    e_phnum,
    e_phoff,
    is64bit,
  }
}
