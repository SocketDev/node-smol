import path from 'node:path'

import { parsePatchFileModifications } from 'build-infra/lib/patch-validator'

export interface NodeSourcePatch {
  content: string
  name: string
}

/**
 * Require every discovered Node patch to own one canonical, unique target.
 */
export function validateNodePatchLayout(
  patches: readonly NodeSourcePatch[],
): void {
  const targetOwners = new Map<string, string>()
  const patchNames = new Set<string>()

  for (let i = 0, { length } = patches; i < length; i += 1) {
    const patch = patches[i]
    if (patch === undefined) {
      continue
    }

    if (patchNames.has(patch.name)) {
      throw new Error(`Node patch name appears more than once: ${patch.name}`)
    }
    patchNames.add(patch.name)

    if (
      typeof patch.content !== 'string' ||
      patch.content.trim().length === 0
    ) {
      throw new Error(`Node patch has no diff content: ${patch.name}`)
    }

    const targets = parseNodePatchTargets(patch.name, patch.content)
    const modifications = parsePatchFileModifications(
      sanitizeHunkFileHeaderText(patch.content),
    )
    if (
      targets.length !== 1 ||
      modifications.size !== 1 ||
      modifications.has(targets[0] ?? '') === false
    ) {
      throw new Error(
        `Node patch must modify exactly one parsed target file: ${patch.name}`,
      )
    }

    const target = targets[0]
    if (target === undefined) {
      throw new Error(`Node patch has no target file: ${patch.name}`)
    }

    const previousOwner = targetOwners.get(target)
    if (previousOwner !== undefined) {
      throw new Error(
        `Node patches ${previousOwner} and ${patch.name} both target ${target}; each target must have one patch owner`,
      )
    }
    targetOwners.set(target, patch.name)
  }
}

function parseNodePatchTargets(patchName: string, content: string): string[] {
  const targets: string[] = []
  const lines = content.split(/\r?\n/)
  let currentTarget: string | undefined
  let oldLinesRemaining = 0
  let newLinesRemaining = 0

  for (let i = 0, { length } = lines; i < length; i += 1) {
    const line = lines[i]
    if (line === undefined) {
      continue
    }

    if (oldLinesRemaining > 0 || newLinesRemaining > 0) {
      if (line.startsWith(' ')) {
        oldLinesRemaining -= 1
        newLinesRemaining -= 1
      } else if (line.startsWith('-')) {
        oldLinesRemaining -= 1
      } else if (line.startsWith('+')) {
        newLinesRemaining -= 1
      } else if (!line.startsWith('\\')) {
        throw new Error(
          `Malformed unified-diff hunk in Node patch: ${patchName}`,
        )
      }

      if (oldLinesRemaining < 0 || newLinesRemaining < 0) {
        throw new Error(
          `Hunk line counts exceed their header in Node patch: ${patchName}`,
        )
      }
      continue
    }

    if (line.startsWith('--- ')) {
      const addedHeader = lines[i + 1]
      if (addedHeader === undefined || !addedHeader.startsWith('+++ ')) {
        throw new Error(`Node patch has an unmatched file header: ${patchName}`)
      }

      const oldTarget = normalizeNodePatchPath(line.slice(4), 'a/', patchName)
      const newTarget = normalizeNodePatchPath(
        addedHeader.slice(4),
        'b/',
        patchName,
      )
      if (oldTarget !== newTarget) {
        throw new Error(
          `Node patch renames or aliases its target: ${patchName}`,
        )
      }
      targets.push(oldTarget)
      currentTarget = oldTarget
      i += 1
      continue
    }

    if (line.startsWith('+++ ')) {
      throw new Error(
        `Node patch has an unmatched added-file header: ${patchName}`,
      )
    }

    if (line.startsWith('@@')) {
      if (currentTarget === undefined) {
        throw new Error(
          `Node patch has a hunk without a file target: ${patchName}`,
        )
      }
      const match = /^@@ -(\d+)(?:,(\d+))? \+(\d+)(?:,(\d+))? @@/.exec(line)
      if (match === null) {
        throw new Error(
          `Malformed unified-diff hunk header in Node patch: ${patchName}`,
        )
      }
      oldLinesRemaining = Number.parseInt(match[2] ?? '1', 10)
      newLinesRemaining = Number.parseInt(match[4] ?? '1', 10)
      if (oldLinesRemaining === 0 && newLinesRemaining === 0) {
        throw new Error(`Empty unified-diff hunk in Node patch: ${patchName}`)
      }
      continue
    }

    if (line.startsWith(' ') || line.startsWith('+') || line.startsWith('-')) {
      throw new Error(
        `Diff content appears outside a hunk in Node patch: ${patchName}`,
      )
    }
  }

  if (oldLinesRemaining > 0 || newLinesRemaining > 0) {
    throw new Error(`Truncated unified-diff hunk in Node patch: ${patchName}`)
  }

  return targets
}

function sanitizeHunkFileHeaderText(content: string): string {
  const lines = content.split(/\r?\n/)
  let oldLinesRemaining = 0
  let newLinesRemaining = 0

  for (let i = 0, { length } = lines; i < length; i += 1) {
    const line = lines[i]
    if (line === undefined) {
      continue
    }

    if (oldLinesRemaining > 0 || newLinesRemaining > 0) {
      if (line.startsWith(' ')) {
        oldLinesRemaining -= 1
        newLinesRemaining -= 1
      } else if (line.startsWith('-')) {
        oldLinesRemaining -= 1
        if (line.startsWith('--- a/')) {
          lines[i] = `-${line}`
        }
      } else if (line.startsWith('+')) {
        newLinesRemaining -= 1
      }
      continue
    }

    if (line.startsWith('@@')) {
      const match = /^@@ -(\d+)(?:,(\d+))? \+(\d+)(?:,(\d+))? @@/.exec(line)
      if (match !== null) {
        oldLinesRemaining = Number.parseInt(match[2] ?? '1', 10)
        newLinesRemaining = Number.parseInt(match[4] ?? '1', 10)
      }
    }
  }

  return lines.join('\n')
}

function normalizeNodePatchPath(
  headerPath: string,
  prefix: 'a/' | 'b/',
  patchName: string,
): string {
  if (!headerPath.startsWith(prefix)) {
    throw new Error(
      `Node patch has an invalid ${prefix} file path: ${patchName}`,
    )
  }

  const filePath = headerPath.slice(prefix.length)
  const normalizedPath = path.posix.normalize(filePath)
  if (
    filePath.length === 0 ||
    filePath !== normalizedPath ||
    filePath === '.' ||
    path.posix.isAbsolute(filePath) ||
    filePath.startsWith('../') ||
    filePath.includes('\\') ||
    /[\s\u0000-\u001f\u007f"']/.test(filePath) ||
    /^[A-Za-z]:/.test(filePath)
  ) {
    throw new Error(`Node patch has a non-canonical target path: ${patchName}`)
  }

  return normalizedPath
}
