// max-file-lines: bindings -- one native-addon wrapper module (Buffer/TextBuffer/EditBuffer/EditorView/Renderer classes over the shared FFI handle); splitting would fracture the handle-lifecycle contract shared across classes
import { existsSync } from 'node:fs'
import { createRequire } from 'node:module'
import path from 'node:path'
import process from 'node:process'
import { fileURLToPath } from 'node:url'

import { spawnSync } from '@socketsecurity/lib-stable/process/spawn/child'

const __dirname = path.dirname(fileURLToPath(import.meta.url))
const esmRequire = createRequire(import.meta.url)

export type NativeHandle = unknown

export type ZigArchKey = NodeJS.Architecture | `${NodeJS.Architecture}_musl`

const PLATFORM_MAP: { __proto__: null } & Partial<
  Record<
    NodeJS.Platform,
    { __proto__: null } & Partial<Record<ZigArchKey, string>>
  >
> = {
  __proto__: null,
  darwin: { __proto__: null, arm64: 'aarch64-macos', x64: 'x86_64-macos' },
  linux: {
    __proto__: null,
    arm64: 'aarch64-linux-gnu',
    arm64_musl: 'aarch64-linux-musl',
    x64: 'x86_64-linux-gnu',
    x64_musl: 'x86_64-linux-musl',
  },
  win32: {
    __proto__: null,
    arm64: 'aarch64-windows-gnu',
    x64: 'x86_64-windows-gnu',
  },
}

const EXT_MAP: { __proto__: null } & Partial<Record<NodeJS.Platform, string>> =
  { __proto__: null, darwin: 'dylib', linux: 'so', win32: 'dll' }
const PREFIX_MAP: { __proto__: null } & Partial<
  Record<NodeJS.Platform, string>
> = { __proto__: null, darwin: 'lib', linux: 'lib', win32: '' }

const PLATFORM_ARCH_MAP: { __proto__: null } & Partial<
  Record<NodeJS.Platform, string>
> = {
  __proto__: null,
  darwin: 'darwin',
  linux: 'linux',
  win32: 'win32',
}

// Optional-symbol probes, memoized on first use. As module-scope constants
// these touched `native` at import time, which defeated the lazy load and made
// merely importing this file require a built artifact.
export interface NativeCapabilities {
  binary: boolean
  cursorInto: boolean
  fa: boolean
  fast: boolean
  sized: boolean
}
let capabilities: NativeCapabilities | undefined
export function caps(): NativeCapabilities {
  capabilities ??= {
    binary: typeof native.writeOutBinary === 'function',
    cursorInto: typeof native.editBufferGetCursorInto === 'function',
    fa: typeof native.bufferDrawTextFA === 'function',
    fast: typeof native.editBufferInsertTextFast === 'function',
    sized: typeof native.editBufferGetTextSized === 'function',
  }
  return capabilities
}

export function detectMusl() {
  if (process.platform !== 'linux') {
    return false
  }
  try {
    // Node reports musl libc via process.report
    const report = process.report?.getReport()
    if (typeof report === 'object' && report !== undefined) {
      const header = (
        report as {
          header?: { glibcVersionRuntime?: unknown | undefined } | undefined
        }
      ).header
      if (header && typeof header.glibcVersionRuntime === 'string') {
        return false
      }
    }
  } catch {}
  // Fallback: check if ldd is musl-based
  try {
    const result = spawnSync('ldd', ['--version'], {
      encoding: 'utf8',
      stdio: ['ignore', 'pipe', 'pipe'],
    })
    const stdout = result.stdout ?? ''
    const stderr = result.stderr ?? ''
    if (/musl/i.test(stdout) || /musl/i.test(stderr)) {
      return true
    }
  } catch {
    // Ignore errors — ldd may not be present on non-glibc/non-musl systems.
  }
  return false
}

/**
 * Has the Zig build produced a loadable native module for this platform?
 *
 * Lets a caller — or a suite of FFI tests — decide to skip rather than throw
 * when the artifact has not been built yet.
 */
export function isNativeModuleBuilt(): boolean {
  try {
    nativeModule()
    return true
  } catch {
    return false
  }
}

export function loadNativeModule() {
  const { platform, arch } = process
  const platformMap = PLATFORM_MAP[platform]
  if (!platformMap) {
    throw new Error(`Unsupported platform: ${platform}`)
  }

  const isMusl = detectMusl()
  const archKey = isMusl ? `${arch}_musl` : arch
  const zigTarget = platformMap[archKey] ?? platformMap[arch]
  if (!zigTarget) {
    throw new Error(`Unsupported architecture: ${platform}-${arch}`)
  }

  const osPart = PLATFORM_ARCH_MAP[platform]
  if (osPart === undefined) {
    throw new Error(
      `OpenTUI loader: unsupported platform '${platform}' — expected one of darwin, linux, win32. Build on a supported platform or add a PLATFORM_ARCH_MAP entry.`,
    )
  }
  const platformArch = `${osPart}-${arch}${isMusl ? '-musl' : ''}`

  const candidates = [
    path.join(
      __dirname,
      '..',
      'build',
      'dev',
      platformArch,
      'out',
      platformArch,
      'opentui.node',
    ),
    path.join(
      __dirname,
      '..',
      'build',
      'prod',
      platformArch,
      'out',
      platformArch,
      'opentui.node',
    ),
  ]

  for (let i = 0, { length } = candidates; i < length; i += 1) {
    const candidate = candidates[i]
    if (candidate === undefined) {
      continue
    }
    if (existsSync(candidate)) {
      return esmRequire(candidate)
    }
  }

  // Raw shared library via dlopen (lib/ directory contains .dylib/.so/.dll
  // built with napi symbols, loadable directly)
  const ext = EXT_MAP[platform]
  const prefix = PREFIX_MAP[platform]
  if (ext === undefined || prefix === undefined) {
    throw new Error(
      `OpenTUI loader: no shared-library naming entry for platform '${platform}' — expected one of darwin, linux, win32. Build on a supported platform or add EXT_MAP/PREFIX_MAP entries.`,
    )
  }
  const libPath = path.join(__dirname, zigTarget, `${prefix}opentui.${ext}`)
  if (existsSync(libPath)) {
    const mod = { __proto__: null, exports: { __proto__: null } }
    process.dlopen(mod, libPath)
    return mod.exports
  }

  throw new Error(
    `OpenTUI native module not found. Searched:\n${[...candidates, libPath].join('\n')}\nRun "pnpm --filter opentui-builder build" to compile.`,
  )
}

let loadedNative: object | undefined

// `loadNativeModule` hands back `any` from a runtime require, so narrow once
// here with a guard instead of asserting off `any` at each use.
export function nativeModule(): object {
  if (loadedNative === undefined) {
    const mod: unknown = loadNativeModule()
    if (typeof mod !== 'object' || mod === null) {
      throw new TypeError(
        'OpenTUI native module loaded but is not an object — the build produced an unexpected artifact.',
      )
    }
    loadedNative = mod
  }
  return loadedNative
}

// Lazy on first property access. `loadNativeModule()` at module scope made
// merely IMPORTING this file throw when the artifact was absent, which took the
// FFI suites down at load and would break any consumer that never touches
// `native` — including the ensure/build scripts that run BEFORE it is compiled.
// oxlint-disable-next-line typescript/no-unsafe-type-assertion -- the Proxy needs a target of the module's own type, which the runtime require gives as `any`. Narrowing it here would retype every native call site in this file; the guard in nativeModule() is what actually validates the load.
export const native = new Proxy({} as ReturnType<typeof loadNativeModule>, {
  get(_target, prop) {
    return Reflect.get(nativeModule(), prop)
  },
})

export const WidthMethod = {
  __proto__: null,
  NO_ZWJ: 2,
  UNICODE: 1,
  WCWIDTH: 0,
}

export const WrapMode = { __proto__: null, CHAR: 1, NONE: 0, WORD: 2 }

export const TextAttributes = {
  __proto__: null,
  BLINK: 16,
  BOLD: 1,
  DIM: 2,
  HIDDEN: 64,
  INVERSE: 32,
  ITALIC: 4,
  NONE: 0,
  STRIKETHROUGH: 128,
  UNDERLINE: 8,
}

export const ATTRIBUTE_BASE_BITS = 8
export const ATTRIBUTE_BASE_MASK = 0xff

export interface Float32x4 extends Float32Array {
  0: number
  1: number
  2: number
  3: number
}

export class RGBA {
  declare buffer: Float32x4

  constructor(r: number, g: number, b: number, a = 1) {
    // oxlint-disable-next-line typescript/no-unsafe-type-assertion -- Float32x4 only brands a 4-lane view over the same bytes; the length is fixed by the literal above.
    this.buffer = new Float32Array([r, g, b, a]) as Float32x4
  }

  get r(): number {
    return this.buffer[0]
  }
  set r(v: number) {
    this.buffer[0] = v
  }

  get g(): number {
    return this.buffer[1]
  }
  set g(v: number) {
    this.buffer[1] = v
  }

  get b(): number {
    return this.buffer[2]
  }
  set b(v: number) {
    this.buffer[2] = v
  }

  get a(): number {
    return this.buffer[3]
  }
  set a(v: number) {
    this.buffer[3] = v
  }

  static fromValues(r: number, g: number, b: number, a = 1): RGBA {
    return new RGBA(r, g, b, a)
  }

  static fromInts(r: number, g: number, b: number, a = 255): RGBA {
    return new RGBA(r / 255, g / 255, b / 255, a / 255)
  }

  static fromHex(hex: string): RGBA {
    hex = hex.replace(/^#/, '')
    if (hex.length === 3) {
      hex =
        hex.charAt(0) +
        hex.charAt(0) +
        hex.charAt(1) +
        hex.charAt(1) +
        hex.charAt(2) +
        hex.charAt(2)
    } else if (hex.length === 4) {
      hex =
        hex.charAt(0) +
        hex.charAt(0) +
        hex.charAt(1) +
        hex.charAt(1) +
        hex.charAt(2) +
        hex.charAt(2) +
        hex.charAt(3) +
        hex.charAt(3)
    }
    if (!/^[0-9A-Fa-f]{6}$/.test(hex) && !/^[0-9A-Fa-f]{8}$/.test(hex)) {
      return new RGBA(1, 0, 1, 1)
    }
    const r = parseInt(hex.substring(0, 2), 16) / 255
    const g = parseInt(hex.substring(2, 4), 16) / 255
    const b = parseInt(hex.substring(4, 6), 16) / 255
    const a = hex.length === 8 ? parseInt(hex.substring(6, 8), 16) / 255 : 1
    return new RGBA(r, g, b, a)
  }

  static fromArray(array: Float32Array | readonly number[]): RGBA {
    const rgba = new RGBA(0, 0, 0, 1)
    // oxlint-disable-next-line typescript/no-unsafe-type-assertion -- Float32x4 only brands a 4-lane view over the same bytes.
    rgba.buffer = (
      array instanceof Float32Array ? array : new Float32Array(array)
    ) as Float32x4
    return rgba
  }

  toInts(): number[] {
    return [
      Math.round(this.r * 255),
      Math.round(this.g * 255),
      Math.round(this.b * 255),
      Math.round(this.a * 255),
    ]
  }

  toHex(): string {
    const components =
      this.a === 1 ? [this.r, this.g, this.b] : [this.r, this.g, this.b, this.a]
    return (
      '#' +
      components
        .map(x => {
          const h = Math.floor(Math.max(0, Math.min(1, x) * 255)).toString(16)
          return h.length === 1 ? '0' + h : h
        })
        .join('')
    )
  }

  equals(other: RGBA | undefined): boolean {
    if (!other) {
      return false
    }
    return (
      this.r === other.r &&
      this.g === other.g &&
      this.b === other.b &&
      this.a === other.a
    )
  }

  toString(): string {
    return `rgba(${this.r.toFixed(2)}, ${this.g.toFixed(2)}, ${this.b.toFixed(2)}, ${this.a.toFixed(2)})`
  }
}

export const DebugOverlayCorner = {
  __proto__: null,
  BOTTOM_LEFT: 2,
  BOTTOM_RIGHT: 3,
  TOP_LEFT: 0,
  TOP_RIGHT: 1,
}

export const TargetChannel = {
  __proto__: null,
  BG: 2,
  BOTH: 3,
  FG: 1,
}

// ── Performance helpers ──

const textEncoder = new TextEncoder()

// oxlint-disable-next-line socket/sort-source-methods -- public API surface ordered by usage (createRenderer first, then helpers); alphabetizing would bury the entry-point function.
export function encodeText(text: string): Uint8Array {
  return textEncoder.encode(text)
}

export class BufferView {
  #ptr: NativeHandle
  #chars: Uint32Array | undefined
  #fg: Float32Array | undefined
  #bg: Float32Array | undefined
  #attrs: Uint32Array | undefined
  #width: number
  #height: number

  constructor(bufferPtr: NativeHandle) {
    this.#ptr = bufferPtr
    this.#chars = undefined
    this.#fg = undefined
    this.#bg = undefined
    this.#attrs = undefined
    this.#width = native.getBufferWidth(bufferPtr)
    this.#height = native.getBufferHeight(bufferPtr)
  }

  get width(): number {
    return this.#width
  }
  get height(): number {
    return this.#height
  }

  get chars(): Uint32Array {
    if (!this.#chars) {
      this.#chars = new Uint32Array(native.bufferGetCharArrayBuffer(this.#ptr))
    }
    return this.#chars
  }

  get fg(): Float32Array {
    if (!this.#fg) {
      this.#fg = new Float32Array(native.bufferGetFgArrayBuffer(this.#ptr))
    }
    return this.#fg
  }

  get bg(): Float32Array {
    if (!this.#bg) {
      this.#bg = new Float32Array(native.bufferGetBgArrayBuffer(this.#ptr))
    }
    return this.#bg
  }

  get attributes(): Uint32Array {
    if (!this.#attrs) {
      this.#attrs = new Uint32Array(
        native.bufferGetAttributesArrayBuffer(this.#ptr),
      )
    }
    return this.#attrs
  }

  setCell(
    x: number,
    y: number,
    char: number,
    fgR: number,
    fgG: number,
    fgB: number,
    fgA: number,
    bgR: number,
    bgG: number,
    bgB: number,
    bgA: number,
    attrs: number,
  ) {
    const idx = y * this.#width + x
    this.chars[idx] = char
    const ci = idx * 4
    this.fg[ci] = fgR
    this.fg[ci + 1] = fgG
    this.fg[ci + 2] = fgB
    this.fg[ci + 3] = fgA
    this.bg[ci] = bgR
    this.bg[ci + 1] = bgG
    this.bg[ci + 2] = bgB
    this.bg[ci + 3] = bgA
    this.attributes[idx] = attrs
  }

  invalidate() {
    this.#chars = undefined
    this.#fg = undefined
    this.#bg = undefined
    this.#attrs = undefined
  }
}

export class CursorState {
  #buf: Uint32Array
  #i32buf: Int32Array

  constructor() {
    this.#buf = new Uint32Array(2)
    this.#i32buf = new Int32Array(3)
  }

  readEditBuffer(editBufferPtr: NativeHandle): this {
    native.editBufferGetCursorInto(editBufferPtr, this.#buf)
    return this
  }

  readEditorView(editorViewPtr: NativeHandle): this {
    native.editorViewGetCursorInto(editorViewPtr, this.#buf)
    return this
  }

  readRenderer(rendererPtr: NativeHandle): this {
    native.getCursorStateInto(rendererPtr, this.#i32buf)
    return this
  }

  get row(): number {
    return this.#buf[0] as number
  }
  get col(): number {
    return this.#buf[1] as number
  }
  get x(): number {
    return this.#i32buf[0] as number
  }
  get y(): number {
    return this.#i32buf[1] as number
  }
  get visible(): boolean {
    return this.#i32buf[2] !== 0
  }
}

// ── High-level API. These use the fast paths internally. ──

const BLACK = new Float32Array([0, 0, 0, 1])
const WHITE = new Float32Array([1, 1, 1, 1])
const TRANSPARENT = new Float32Array([0, 0, 0, 0])

export type ColorInput = Float32Array | RGBA

// oxlint-disable-next-line socket/sort-source-methods -- public API surface ordered by usage (createRenderer first, then helpers); alphabetizing would bury the entry-point function.
export function colorBuf(color: ColorInput): Float32Array {
  if (color instanceof RGBA) {
    return color.buffer
  }
  if (color instanceof Float32Array) {
    return color
  }
  return BLACK
}

export interface BufferOptions {
  id?: string | undefined
  respectAlpha?: boolean | undefined
  widthMethod?: number | undefined
}

export class Buffer {
  #ptr: NativeHandle
  #view: BufferView | undefined

  constructor(
    width: number,
    height: number,
    options?: BufferOptions | undefined,
  ) {
    const opts = { __proto__: null, ...options } as typeof options
    const widthMethod = opts?.widthMethod ?? WidthMethod.WCWIDTH
    const id = opts?.id ?? ''
    const respectAlpha = opts?.respectAlpha ?? false
    this.#ptr = native.createOptimizedBuffer(
      width,
      height,
      respectAlpha,
      widthMethod,
      id,
    )
    this.#view = undefined
  }

  get ptr(): NativeHandle {
    return this.#ptr
  }
  get width(): number {
    return native.getBufferWidth(this.#ptr)
  }
  get height(): number {
    return native.getBufferHeight(this.#ptr)
  }

  get view(): BufferView {
    if (!this.#view) {
      this.#view = new BufferView(this.#ptr)
    }
    return this.#view
  }

  clear(bg?: ColorInput | undefined) {
    if (caps().fa && bg) {
      const b = colorBuf(bg)
      native.bufferClear(this.#ptr, b[0], b[1], b[2], b[3])
    } else {
      native.bufferClear(this.#ptr, 0, 0, 0, 1)
    }
    if (this.#view) {
      this.#view.invalidate()
    }
  }

  resize(width: number, height: number) {
    native.bufferResize(this.#ptr, width, height)
    if (this.#view) {
      this.#view.invalidate()
    }
  }

  drawText(
    text: string,
    x: number,
    y: number,
    fg?: ColorInput | undefined,
    bg?: ColorInput | undefined,
    attrs = 0,
  ) {
    const fgBuf = colorBuf(fg ?? WHITE)
    const bgBuf = colorBuf(bg ?? BLACK)
    if (caps().fa) {
      native.bufferDrawTextFA(this.#ptr, text, x, y, fgBuf, bgBuf, attrs)
    } else {
      native.bufferDrawText(
        this.#ptr,
        text,
        x,
        y,
        fgBuf[0],
        fgBuf[1],
        fgBuf[2],
        fgBuf[3],
        bgBuf[0],
        bgBuf[1],
        bgBuf[2],
        bgBuf[3],
        attrs,
      )
    }
  }

  drawChar(
    char: number,
    x: number,
    y: number,
    fg?: ColorInput | undefined,
    bg?: ColorInput | undefined,
    attrs = 0,
  ) {
    const fgBuf = colorBuf(fg ?? WHITE)
    const bgBuf = colorBuf(bg ?? BLACK)
    if (caps().fa) {
      native.bufferDrawCharFA(this.#ptr, char, x, y, fgBuf, bgBuf, attrs)
    } else {
      native.bufferDrawChar(
        this.#ptr,
        char,
        x,
        y,
        fgBuf[0],
        fgBuf[1],
        fgBuf[2],
        fgBuf[3],
        bgBuf[0],
        bgBuf[1],
        bgBuf[2],
        bgBuf[3],
        attrs,
      )
    }
  }

  setCell(
    x: number,
    y: number,
    char: number,
    fg?: ColorInput | undefined,
    bg?: ColorInput | undefined,
    attrs = 0,
  ) {
    const fgBuf = colorBuf(fg ?? WHITE)
    const bgBuf = colorBuf(bg ?? BLACK)
    if (caps().fa) {
      native.bufferSetCellFA(this.#ptr, x, y, char, fgBuf, bgBuf)
    } else {
      native.bufferSetCell(
        this.#ptr,
        x,
        y,
        char,
        fgBuf[0],
        fgBuf[1],
        fgBuf[2],
        fgBuf[3],
        bgBuf[0],
        bgBuf[1],
        bgBuf[2],
        bgBuf[3],
        attrs,
      )
    }
  }

  fillRect(
    x: number,
    y: number,
    width: number,
    height: number,
    bg?: ColorInput | undefined,
  ) {
    const bgBuf = colorBuf(bg ?? BLACK)
    if (caps().fa) {
      native.bufferFillRectFA(this.#ptr, x, y, width, height, bgBuf)
    } else {
      native.bufferFillRect(
        this.#ptr,
        x,
        y,
        width,
        height,
        bgBuf[0],
        bgBuf[1],
        bgBuf[2],
        bgBuf[3],
      )
    }
  }

  pushScissorRect(x: number, y: number, width: number, height: number) {
    native.bufferPushScissorRect(this.#ptr, x, y, width, height)
  }

  popScissorRect() {
    native.bufferPopScissorRect(this.#ptr)
  }
  clearScissorRects() {
    native.bufferClearScissorRects(this.#ptr)
  }

  pushOpacity(opacity: number) {
    native.bufferPushOpacity(this.#ptr, opacity)
  }
  popOpacity() {
    native.bufferPopOpacity(this.#ptr)
  }
  get opacity(): number {
    return native.bufferGetCurrentOpacity(this.#ptr)
  }

  drawEditorView(editorView: EditorView | NativeHandle, x: number, y: number) {
    const ptr = toHandle(editorView)
    native.bufferDrawEditorView(this.#ptr, ptr, x, y)
  }

  drawTextBufferView(textBufferView: NativeHandle, x: number, y: number) {
    const ptr = toHandle(textBufferView)
    native.bufferDrawTextBufferView(this.#ptr, ptr, x, y)
  }

  destroy() {
    native.destroyOptimizedBuffer(this.#ptr)
    this.#view = undefined
  }
}

export class TextBuffer {
  #ptr: NativeHandle

  constructor(widthMethod = WidthMethod.WCWIDTH) {
    this.#ptr = native.createTextBuffer(widthMethod)
  }

  get ptr(): NativeHandle {
    return this.#ptr
  }
  get length(): number {
    return native.textBufferGetLength(this.#ptr)
  }
  get byteSize(): number {
    return native.textBufferGetByteSize(this.#ptr)
  }
  get lineCount(): number {
    return native.textBufferGetLineCount(this.#ptr)
  }

  get text(): string {
    return caps().sized
      ? native.textBufferGetPlainTextSized(this.#ptr)
      : native.textBufferGetPlainText(this.#ptr)
  }

  append(text: string) {
    if (caps().fast) {
      native.textBufferAppendFast(this.#ptr, text)
    } else {
      native.textBufferAppend(this.#ptr, text)
    }
  }

  clear() {
    native.textBufferClear(this.#ptr)
  }
  reset() {
    native.textBufferReset(this.#ptr)
  }

  setDefaultFg(color: ColorInput) {
    const c = colorBuf(color)
    native.textBufferSetDefaultFg(this.#ptr, c[0], c[1], c[2], c[3])
  }

  setDefaultBg(color: ColorInput) {
    const c = colorBuf(color)
    native.textBufferSetDefaultBg(this.#ptr, c[0], c[1], c[2], c[3])
  }

  setDefaultAttributes(attrs: number) {
    native.textBufferSetDefaultAttributes(this.#ptr, attrs)
  }

  resetDefaults() {
    native.textBufferResetDefaults(this.#ptr)
  }

  get tabWidth(): number {
    return native.textBufferGetTabWidth(this.#ptr)
  }
  set tabWidth(w: number) {
    native.textBufferSetTabWidth(this.#ptr, w)
  }

  addHighlight(
    lineIdx: number,
    start: number,
    end: number,
    styleId: number,
    priority = 0,
    hlRef = 0,
  ) {
    native.textBufferAddHighlight(
      this.#ptr,
      lineIdx,
      start,
      end,
      styleId,
      priority,
      hlRef,
    )
  }

  removeHighlightsByRef(hlRef: number) {
    native.textBufferRemoveHighlightsByRef(this.#ptr, hlRef)
  }
  clearAllHighlights() {
    native.textBufferClearAllHighlights(this.#ptr)
  }

  setSyntaxStyle(style: SyntaxStyle | NativeHandle) {
    const ptr = toHandle(style)
    native.textBufferSetSyntaxStyle(this.#ptr, ptr)
  }

  destroy() {
    native.destroyTextBuffer(this.#ptr)
  }
}

export class EditBuffer {
  #ptr: NativeHandle
  #cursor: CursorState | undefined

  constructor(widthMethod = WidthMethod.WCWIDTH) {
    this.#ptr = native.createEditBuffer(widthMethod)
    this.#cursor = caps().cursorInto ? new CursorState() : undefined
  }

  get ptr(): NativeHandle {
    return this.#ptr
  }

  get text(): string {
    return caps().sized
      ? native.editBufferGetTextSized(this.#ptr)
      : native.editBufferGetText(this.#ptr)
  }

  set text(value: string) {
    native.editBufferSetText(this.#ptr, value)
  }

  insertText(text: string) {
    if (caps().fast) {
      native.editBufferInsertTextFast(this.#ptr, text)
    } else {
      native.editBufferInsertText(this.#ptr, text)
    }
  }

  get cursor(): CursorState {
    if (this.#cursor) {
      return this.#cursor.readEditBuffer(this.#ptr)
    }
    return native.editBufferGetCursor(this.#ptr)
  }

  setCursor(row: number, col: number) {
    native.editBufferSetCursor(this.#ptr, row, col)
  }
  setCursorByOffset(offset: number) {
    native.editBufferSetCursorByOffset(this.#ptr, offset)
  }
  gotoLine(line: number) {
    native.editBufferGotoLine(this.#ptr, line)
  }

  moveCursorLeft() {
    native.editBufferMoveCursorLeft(this.#ptr)
  }
  moveCursorRight() {
    native.editBufferMoveCursorRight(this.#ptr)
  }
  moveCursorUp() {
    native.editBufferMoveCursorUp(this.#ptr)
  }
  moveCursorDown() {
    native.editBufferMoveCursorDown(this.#ptr)
  }

  deleteChar() {
    native.editBufferDeleteChar(this.#ptr)
  }
  deleteCharBackward() {
    native.editBufferDeleteCharBackward(this.#ptr)
  }
  deleteRange(
    startRow: number,
    startCol: number,
    endRow: number,
    endCol: number,
  ) {
    native.editBufferDeleteRange(this.#ptr, startRow, startCol, endRow, endCol)
  }
  deleteLine() {
    native.editBufferDeleteLine(this.#ptr)
  }
  newLine() {
    native.editBufferNewLine(this.#ptr)
  }

  undo() {
    native.editBufferUndo(this.#ptr)
  }
  redo() {
    native.editBufferRedo(this.#ptr)
  }
  get canUndo(): boolean {
    return native.editBufferCanUndo(this.#ptr)
  }
  get canRedo(): boolean {
    return native.editBufferCanRedo(this.#ptr)
  }
  clearHistory() {
    native.editBufferClearHistory(this.#ptr)
  }

  getTextRange(startOffset: number, endOffset: number): string {
    return native.editBufferGetTextRange(this.#ptr, startOffset, endOffset)
  }

  getTextRangeByCoords(
    startRow: number,
    startCol: number,
    endRow: number,
    endCol: number,
  ): string {
    return native.editBufferGetTextRangeByCoords(
      this.#ptr,
      startRow,
      startCol,
      endRow,
      endCol,
    )
  }

  clear() {
    native.editBufferClear(this.#ptr)
  }
  destroy() {
    native.destroyEditBuffer(this.#ptr)
  }
}

export class EditorView {
  #ptr: NativeHandle
  #cursor: CursorState | undefined

  constructor(
    editBuffer: EditBuffer | NativeHandle,
    width: number,
    height: number,
  ) {
    const ptr = toHandle(editBuffer)
    this.#ptr = native.createEditorView(ptr, width, height)
    this.#cursor = caps().cursorInto ? new CursorState() : undefined
  }

  get ptr(): NativeHandle {
    return this.#ptr
  }

  get cursor(): CursorState {
    if (this.#cursor) {
      return this.#cursor.readEditorView(this.#ptr)
    }
    return native.editorViewGetCursor(this.#ptr)
  }

  get visualCursor() {
    return native.editorViewGetVisualCursor(this.#ptr)
  }
  get viewport() {
    return native.editorViewGetViewport(this.#ptr)
  }
  get virtualLineCount(): number {
    return native.editorViewGetVirtualLineCount(this.#ptr)
  }
  get totalVirtualLineCount(): number {
    return native.editorViewGetTotalVirtualLineCount(this.#ptr)
  }
  get text(): string {
    return native.editorViewGetText(this.#ptr)
  }

  setViewportSize(width: number, height: number) {
    native.editorViewSetViewportSize(this.#ptr, width, height)
  }
  setViewport(
    x: number,
    y: number,
    width: number,
    height: number,
    moveCursor = false,
  ) {
    native.editorViewSetViewport(this.#ptr, x, y, width, height, moveCursor)
  }
  clearViewport() {
    native.editorViewClearViewport(this.#ptr)
  }
  setScrollMargin(margin: number) {
    native.editorViewSetScrollMargin(this.#ptr, margin)
  }
  setWrapMode(mode: number) {
    native.editorViewSetWrapMode(this.#ptr, mode)
  }

  moveUpVisual() {
    native.editorViewMoveUpVisual(this.#ptr)
  }
  moveDownVisual() {
    native.editorViewMoveDownVisual(this.#ptr)
  }

  setSelection(
    start: number,
    end: number,
    bgColor?: ColorInput | undefined,
    fgColor?: ColorInput | undefined,
  ) {
    const bg = colorBuf(bgColor ?? TRANSPARENT)
    const fg = colorBuf(fgColor ?? WHITE)
    native.editorViewSetSelection(
      this.#ptr,
      start,
      end,
      bg[0],
      bg[1],
      bg[2],
      bg[3],
      fg[0],
      fg[1],
      fg[2],
      fg[3],
    )
  }

  resetSelection() {
    native.editorViewResetSelection(this.#ptr)
  }
  deleteSelectedText() {
    native.editorViewDeleteSelectedText(this.#ptr)
  }

  destroy() {
    native.destroyEditorView(this.#ptr)
  }
}

export class SyntaxStyle {
  #ptr: NativeHandle

  constructor() {
    this.#ptr = native.createSyntaxStyle()
  }

  get ptr(): NativeHandle {
    return this.#ptr
  }
  get styleCount(): number {
    return native.syntaxStyleGetStyleCount(this.#ptr)
  }

  register(
    name: string,
    fg?: ColorInput | undefined,
    bg?: ColorInput | undefined,
    attrs = 0,
  ): number {
    const fgBuf = colorBuf(fg ?? TRANSPARENT)
    const bgBuf = colorBuf(bg ?? TRANSPARENT)
    return native.syntaxStyleRegister(
      this.#ptr,
      name,
      fgBuf[0],
      fgBuf[1],
      fgBuf[2],
      fgBuf[3],
      bgBuf[0],
      bgBuf[1],
      bgBuf[2],
      bgBuf[3],
      attrs,
    )
  }

  resolve(name: string): number {
    return native.syntaxStyleResolveByName(this.#ptr, name)
  }
  destroy() {
    native.destroySyntaxStyle(this.#ptr)
  }
}

export interface RendererOptions {
  remote?: boolean | undefined
  testing?: boolean | undefined
}

export class Renderer {
  #ptr: NativeHandle
  #cursor: CursorState | undefined

  constructor(
    width: number,
    height: number,
    options?: RendererOptions | undefined,
  ) {
    const opts = { __proto__: null, ...options } as typeof options
    const testing = opts?.testing ?? false
    const remote = opts?.remote ?? false
    this.#ptr = native.createRenderer(width, height, testing, remote)
    this.#cursor = caps().cursorInto ? new CursorState() : undefined
  }

  get ptr(): NativeHandle {
    return this.#ptr
  }

  get nextBuffer(): NativeHandle {
    return native.getNextBuffer(this.#ptr)
  }
  get currentBuffer(): NativeHandle {
    return native.getCurrentBuffer(this.#ptr)
  }

  render(force = false) {
    native.render(this.#ptr, force)
  }

  resize(width: number, height: number) {
    native.resizeRenderer(this.#ptr, width, height)
  }

  setBackgroundColor(color: ColorInput) {
    const c = colorBuf(color)
    native.setBackgroundColor(this.#ptr, c[0], c[1], c[2], c[3])
  }

  get cursorState(): CursorState {
    if (this.#cursor) {
      return this.#cursor.readRenderer(this.#ptr)
    }
    return native.getCursorState(this.#ptr)
  }

  setCursorPosition(x: number, y: number, visible = true) {
    native.setCursorPosition(this.#ptr, x, y, visible)
  }

  setTerminalTitle(title: string) {
    native.setTerminalTitle(this.#ptr, title)
  }
  clearTerminal() {
    native.clearTerminal(this.#ptr)
  }

  enableMouse(enableMovement = true) {
    native.enableMouse(this.#ptr, enableMovement)
  }
  disableMouse() {
    native.disableMouse(this.#ptr)
  }

  setupTerminal(useAlternateScreen = true) {
    native.setupTerminal(this.#ptr, useAlternateScreen)
  }
  suspend() {
    native.suspendRenderer(this.#ptr)
  }
  resume() {
    native.resumeRenderer(this.#ptr)
  }

  writeOut(data: string | Uint8Array) {
    if (caps().binary && data instanceof Uint8Array) {
      native.writeOutBinary(this.#ptr, data)
    } else {
      native.writeOut(this.#ptr, data)
    }
  }

  addToHitGrid(
    x: number,
    y: number,
    width: number,
    height: number,
    id: number,
  ) {
    native.addToHitGrid(this.#ptr, x, y, width, height, id)
  }

  checkHit(x: number, y: number): number {
    return native.checkHit(this.#ptr, x, y)
  }
  clearHitGrid() {
    native.clearCurrentHitGrid(this.#ptr)
  }

  destroy() {
    native.destroyRenderer(this.#ptr)
  }
}

/**
 * Accept either a wrapper object carrying a native `ptr` or a bare handle, and
 * hand back the handle. The native calls take a handle; the public API lets
 * callers pass the wrapper for convenience.
 */
export function toHandle(value: unknown): NativeHandle {
  return typeof value === 'object' && value !== null && 'ptr' in value
    ? toHandle(value)
    : value
}
