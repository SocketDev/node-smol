/*
 * @file Position-tracked Markdown parsing for the fleet's CHANGELOG tooling.
 *
 *   Structure questions — where a `## ` section starts, whether a block carries
 *   bullets, which `### ` subsection a bullet belongs to — are answered from a
 *   GFM mdast tree, never by scanning raw lines. A line scanner cannot tell a
 *   heading from a `## ` INSIDE a fenced code block, so a changelog that
 *   documents its own markup (a ```md sample, a shell transcript) used to split
 *   into phantom sections and lose entries.
 *
 *   Every helper reports 0-based LINE INDEXES and VERBATIM source slices, so
 *   callers edit the original text at those positions. Nothing round-trips
 *   through a serializer: untouched bytes stay byte-identical, which is the
 *   whole point — a reformatting changelog writer would churn every release.
 */

import { fromMarkdown } from 'mdast-util-from-markdown'
import { gfmFromMarkdown } from 'mdast-util-gfm'
import { gfm } from 'micromark-extension-gfm'

import type { Nodes, Root } from 'mdast'

export interface MarkdownHeading {
  /**
   * ATX (`## x`) or setext (`x\n--`) level, 1 through 6.
   */
  depth: number
  /**
   * 0-based index of the line the heading starts on.
   */
  line: number
  /**
   * Flattened text content — no `#` markers, and a link contributes its label,
   * so `## [1.2.3](url) - 2026-01-01` reads as `1.2.3 - 2026-01-01`.
   */
  text: string
}

/**
 * Parse Markdown the way GitHub renders it (GFM: tables, strikethrough,
 * autolinks, task lists) into a position-tracked mdast tree.
 */
export function parseMarkdown(source: string): Root {
  return fromMarkdown(source, {
    extensions: [gfm()],
    mdastExtensions: [gfmFromMarkdown()],
  })
}

/**
 * A node's flattened text content. Code spans keep their literal value; raw
 * HTML and images contribute nothing, so a heading's text is what a reader
 * sees rather than what the source spells.
 */
export function nodeText(node: Nodes): string {
  switch (node.type) {
    case 'inlineCode':
    case 'text':
      return node.value
    default:
      return 'children' in node ? node.children.map(nodeText).join('') : ''
  }
}

/**
 * Every TOP-LEVEL heading, in source order. Top-level is deliberate: a heading
 * nested in a blockquote or a list item is quoted content, not a section of
 * this document, and treating it as one splits a section in half.
 */
export function documentHeadings(source: string): MarkdownHeading[] {
  const out: MarkdownHeading[] = []
  for (const node of parseMarkdown(source).children) {
    if (node.type === 'heading' && node.position) {
      out.push({
        depth: node.depth,
        line: node.position.start.line - 1,
        text: nodeText(node).trim(),
      })
    }
  }
  return out
}

/**
 * 0-based line indexes of the document's level-`depth` headings — the section
 * boundaries a changelog edit slices on.
 */
export function headingLines(source: string, depth: number): number[] {
  const out: number[] = []
  for (const heading of documentHeadings(source)) {
    if (heading.depth === depth) {
      out.push(heading.line)
    }
  }
  return out
}

function nodeHasListItem(node: Nodes): boolean {
  if (node.type === 'listItem') {
    return true
  }
  return 'children' in node ? node.children.some(nodeHasListItem) : false
}

/**
 * True when the Markdown carries at least one real list item, as opposed to a
 * bare heading with nothing under it. Parsed, not pattern matched: a `- ` line
 * inside a fenced code block is a code sample, not an entry.
 */
export function hasListItem(source: string): boolean {
  return parseMarkdown(source).children.some(nodeHasListItem)
}

/**
 * Every top-level list item in the document, in source order, each as its
 * VERBATIM source slice — ungrouped, for callers that compare a document's
 * entries without regard for which subsection they sit under. An item nested
 * inside another item rides along inside its parent's slice rather than
 * appearing separately.
 */
export function documentListItems(source: string): string[] {
  const out: string[] = []
  for (const node of parseMarkdown(source).children) {
    if (node.type !== 'list') {
      continue
    }
    for (const item of node.children) {
      const start = item.position?.start.offset
      const end = item.position?.end.offset
      if (start !== undefined && end !== undefined) {
        out.push(source.slice(start, end).trimEnd())
      }
    }
  }
  return out
}

/**
 * Top-level list items grouped under the level-`depth` heading they follow,
 * keyed by that heading's text, each item kept as its VERBATIM source slice so
 * a bullet's own markup survives the round trip untouched.
 *
 * A matching heading with no items still gets an (empty) entry, and items
 * before the first matching heading are dropped — they belong to no section.
 * Headings at other depths are passed over without closing the current
 * section, matching how a version section's `### Added` block keeps collecting
 * across nested structure.
 *
 * A WRAPPED bullet (continuation lines indented under the `- `) comes back
 * whole. The line scanner this replaces kept only the line that started with
 * `- ` and silently dropped the rest of the sentence.
 */
export function listItemsByHeading(
  source: string,
  depth: number,
): Map<string, string[]> {
  const out = new Map<string, string[]>()
  let current: string | undefined
  for (const node of parseMarkdown(source).children) {
    if (node.type === 'heading') {
      if (node.depth === depth) {
        current = nodeText(node).trim()
        if (!out.has(current)) {
          out.set(current, [])
        }
      }
      continue
    }
    if (node.type !== 'list' || current === undefined) {
      continue
    }
    const bullets = out.get(current)!
    for (const item of node.children) {
      const start = item.position?.start.offset
      const end = item.position?.end.offset
      if (start !== undefined && end !== undefined) {
        bullets.push(source.slice(start, end).trimEnd())
      }
    }
  }
  return out
}
