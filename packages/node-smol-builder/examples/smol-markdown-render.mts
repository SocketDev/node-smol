#!/usr/bin/env node
/**
 * Node:smol-markdown demo — render AI-style Markdown output to the
 * terminal using node:smol-tui as the renderer.
 *
 * Demonstrates the full Phase B integration: md4c parses input into
 * an event stream, the demo walks events and dispatches each into
 * node:smol-tui's DrawTextWrapped + DrawBox primitives.
 *
 * Run with socket-built node (see smol-tui-hello.mts for the binary
 * path).
 */
import {
  ATTRIBUTE_BASE_MASK,
  createRenderer,
  destroyRenderer,
  rendererClear,
  rendererDrawBox,
  rendererDrawTextWrapped,
  rendererFlush,
  rendererSize,
  stringWidth,
  TextAttributes,
} from 'node:smol-tui'

import {
  blockType,
  eventCategory,
  parseMarkdown,
  spanType,
  textType,
} from 'node:smol-markdown'

const FLUSH_BUF = new Uint8Array(256 * 1024)

const SAMPLE_MD = `# Hello from smol-markdown

This is a **CommonMark + GFM** Markdown parser implemented in C++ via
md4c, exposed as \`node:smol-markdown\` on socket-built Node.

## Features

- Native parsing (no JS regex engine)
- Full GFM dialect: tables, strikethrough, tasklists, autolinks
- Flat event stream — JS reconstructs the tree

## Example code

\`\`\`js
const events = parseMarkdown(text, 'github')
\`\`\`

That's the whole API.
`

const CATEGORY_MASK = 0xf0_00
const VALUE_MASK = 0x0f_ff

export function appendText(
  value: number,
  payload: undefined | string | number,
  buffer: RenderBuffer,
  state: RenderState,
): void {
  if (typeof payload !== 'string') {
    return
  }
  if (value !== textType.CODE || !payload.includes('\n')) {
    buffer.line += payload
    return
  }
  const lines = payload.split(/\r?\n/)
  for (let i = 0, { length } = lines; i < length; i += 1) {
    buffer.line += lines[i]
    if (i < length - 1) {
      flushRenderLine(buffer, state)
    }
  }
}

export function enterBlock(
  value: number,
  payload: undefined | string | number,
  buffer: RenderBuffer,
  state: RenderState,
): void {
  if (value === blockType.H) {
    flushRenderLine(buffer, state)
    state.inHeading = true
    state.headingLevel = typeof payload === 'number' ? payload : 1
    state.fgR = state.headingLevel === 1 ? 255 : 200
    state.fgG = state.headingLevel === 1 ? 200 : 220
    state.fgB = 100
    buffer.attrs = TextAttributes.BOLD
  } else if (value === blockType.CODE) {
    flushRenderLine(buffer, state)
    state.fgR = 150
    state.fgG = 255
    state.fgB = 150
  } else if (value === blockType.LI) {
    flushRenderLine(buffer, state)
    buffer.line = '  • '
  }
}

export function enterSpan(
  value: number,
  buffer: RenderBuffer,
  state: RenderState,
): void {
  if (value === spanType.STRONG) {
    buffer.attrs |= TextAttributes.BOLD
  } else if (value === spanType.EM) {
    buffer.attrs |= TextAttributes.ITALIC
  } else if (value === spanType.CODE) {
    state.fgR = 150
    state.fgG = 255
    state.fgB = 150
  }
}

export interface RenderState {
  y: number
  attrs: number
  fgR: number
  fgG: number
  fgB: number
  inHeading: boolean
  headingLevel: number
  rendererId: number
  width: number
}

export interface RenderBuffer {
  attrs: number
  line: string
}

export function flushRenderLine(
  buffer: RenderBuffer,
  state: RenderState,
): void {
  if (!buffer.line) {
    return
  }
  const bytes = new TextEncoder().encode(buffer.line)
  rendererDrawTextWrapped(
    state.rendererId,
    2,
    state.y,
    Math.max(20, state.width - 4),
    0,
    bytes,
    state.fgR,
    state.fgG,
    state.fgB,
    0,
    0,
    20,
    buffer.attrs & ATTRIBUTE_BASE_MASK,
  )
  state.y += 1
  buffer.line = ''
  buffer.attrs = 0
}

export function leaveBlock(
  value: number,
  buffer: RenderBuffer,
  state: RenderState,
): void {
  flushRenderLine(buffer, state)
  if (value === blockType.H) {
    state.inHeading = false
    state.fgR = 220
    state.fgG = 220
    state.fgB = 220
    state.y += 1
  } else if (value === blockType.CODE) {
    state.fgR = 220
    state.fgG = 220
    state.fgB = 220
    state.y += 1
  } else if (value === blockType.P) {
    state.y += 1
  }
}

export function leaveSpan(
  value: number,
  buffer: RenderBuffer,
  state: RenderState,
): void {
  if (value === spanType.STRONG) {
    buffer.attrs &= ~TextAttributes.BOLD
  } else if (value === spanType.EM) {
    buffer.attrs &= ~TextAttributes.ITALIC
  } else if (value === spanType.CODE) {
    state.fgR = state.inHeading ? 255 : 220
    state.fgG = state.inHeading ? 200 : 220
    state.fgB = state.inHeading ? 100 : 220
  }
}

export function processEvents(
  events: Array<[number, undefined | string | number]>,
  state: RenderState,
): void {
  const buffer: RenderBuffer = { attrs: 0, line: '' }

  for (let i = 0, { length } = events; i < length; i += 1) {
    const { 0: code, 1: payload } = events[i]
    const cat = code & CATEGORY_MASK
    const val = code & VALUE_MASK

    if (cat === eventCategory.BLOCK_ENTER) {
      enterBlock(val, payload, buffer, state)
    } else if (cat === eventCategory.BLOCK_LEAVE) {
      leaveBlock(val, buffer, state)
    } else if (cat === eventCategory.SPAN_ENTER) {
      enterSpan(val, buffer, state)
    } else if (cat === eventCategory.SPAN_LEAVE) {
      leaveSpan(val, buffer, state)
    } else if (cat === eventCategory.TEXT) {
      appendText(val, payload, buffer, state)
    }
  }
  flushRenderLine(buffer, state)
}

export function stdoutWrite(data: Uint8Array | string): void {
  process.stdout.write(data) // socket-hook: allow console
}

function main(): void {
  const cols = process.stdout.columns ?? 80
  const rows = process.stdout.rows ?? 40
  const rendererId = createRenderer(cols, rows, false, false)
  stdoutWrite('\x1b[?1049h\x1b[?25l')

  const exit = (): void => {
    stdoutWrite('\x1b[?25h\x1b[?1049l')
    destroyRenderer(rendererId)
    process.exit(0)
  }
  process.on('SIGINT', exit)
  process.on('SIGTERM', exit)

  rendererClear(rendererId)
  const { width, height } = rendererSize(rendererId)

  // Frame.
  rendererDrawBox(
    rendererId,
    0,
    0,
    width,
    height,
    /* style */ 2, // rounded
    /* sidesBits */ 0xf,
    100,
    200,
    255,
    0,
    0,
    20,
    0,
    true,
  )

  // Parse with GitHub dialect (tables + strikethrough + tasklists +
  // autolinks).
  const events = parseMarkdown(SAMPLE_MD, 'github')

  const state: RenderState = {
    y: 1,
    attrs: 0,
    fgR: 220,
    fgG: 220,
    fgB: 220,
    inHeading: false,
    headingLevel: 0,
    rendererId,
    width,
  }
  processEvents(events, state)

  const bytesWritten = rendererFlush(rendererId, FLUSH_BUF, FLUSH_BUF.length)
  if (bytesWritten > 0 && bytesWritten < FLUSH_BUF.length) {
    stdoutWrite(FLUSH_BUF.subarray(0, bytesWritten))
  }

  // Keep alive until Ctrl-C.
  setInterval(() => {}, 60_000)
}

main()
