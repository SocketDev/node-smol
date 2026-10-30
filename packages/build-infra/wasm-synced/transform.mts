/**
 * Common AST transformations for both CJS and ESM sync wrappers.
 *
 * Applies transformations that are shared between CommonJS and ESM:
 *
 * - Remove export/import statements
 * - Remove async/await keywords
 * - Transform WebAssembly.instantiate to synchronous WebAssembly.Instance
 * - Clean up Promise patterns
 * - Handle Node.js module patterns.
 *
 * Visitor implementations are split into:
 *
 * - Transform-decl-visitor.mts: declaration/function/async handlers
 * - Transform-call-visitor.mts: call/variable/return handlers.
 */

import { Parser } from 'acorn'
import { ancestor, simple as walkSimple } from 'acorn-walk'
import MagicString from 'magic-string'

import { buildCallVisitor } from './transform-call-visitor.mts'
import { buildDeclVisitor } from './transform-decl-visitor.mts'

export function analyzeCountInstantiateParams(node, paramNames) {
  const paramCounts = { __proto__: null }
  let foundInstantiate = false
  walkSimple(node, {
    CallExpression(callNode) {
      if (!analyzeIsAsyncWebAssemblyInstantiation(callNode.callee)) {
        return
      }
      const importsArg = callNode.arguments[1]
      if (importsArg?.type !== 'Identifier') {
        return
      }
      const { name } = importsArg
      if (paramNames.includes(name)) {
        paramCounts[name] = (paramCounts[name] || 0) + 1
        foundInstantiate = true
      }
    },
  })
  return { __proto__: null, foundInstantiate, paramCounts }
}

export function analyzeFindImportsSetup(node, mjsContent) {
  for (const stmt of node.body.body) {
    if (stmt.type !== 'VariableDeclaration') {
      continue
    }
    for (const decl of stmt.declarations) {
      const varName = decl.id?.name
      const initType = decl.init?.type
      if (
        varName &&
        (initType === 'CallExpression' ||
          initType === 'NewExpression' ||
          initType === 'ObjectExpression')
      ) {
        const stmtCode = mjsContent.slice(stmt.start, stmt.end)
        return {
          __proto__: null,
          importsParam: varName,
          importsSetup: stmtCode.endsWith(';') ? stmtCode : `${stmtCode};`,
        }
      }
    }
  }
  return undefined
}

export function analyzeInspectWasmFunction(node) {
  const result = {
    __proto__: null,
    hasAsyncWebAssemblyCall: false,
    hasLoadingMechanism: false,
    isNotAlreadySync: true,
  }

  walkSimple(node, {
    CallExpression(callNode) {
      const { callee } = callNode
      if (analyzeIsAsyncWebAssemblyInstantiation(callee)) {
        result.hasAsyncWebAssemblyCall = true
      }
      if (callee.type === 'Identifier' && callee.name === 'fetch') {
        result.hasLoadingMechanism = true
      }
    },
    Identifier(idNode) {
      if (idNode.name === 'wasmBinary') {
        result.hasLoadingMechanism = true
      }
    },
    NewExpression(newNode) {
      const { callee } = newNode
      if (
        callee.type === 'MemberExpression' &&
        callee.object.type === 'Identifier' &&
        callee.object.name === 'WebAssembly' &&
        callee.property.type === 'Identifier' &&
        callee.property.name === 'Instance'
      ) {
        result.isNotAlreadySync = false
      }
    },
  })
  return result
}

export function analyzeIsAsyncWebAssemblyInstantiation(callee) {
  return (
    callee.type === 'MemberExpression' &&
    callee.object.type === 'Identifier' &&
    callee.object.name === 'WebAssembly' &&
    callee.property.type === 'Identifier' &&
    (callee.property.name === 'instantiate' ||
      callee.property.name === 'instantiateStreaming')
  )
}

export function analyzeResolveImportsBinding(node, paramNames, mjsContent) {
  const { foundInstantiate, paramCounts } = analyzeCountInstantiateParams(
    node,
    paramNames,
  )
  const sortedParams = Object.entries(paramCounts).toSorted(
    ([, firstCount], [, secondCount]) => secondCount - firstCount,
  )
  if (sortedParams.length > 0) {
    return {
      __proto__: null,
      importsParam: sortedParams[0][0],
      importsSetup: '',
    }
  }
  if (!foundInstantiate) {
    const importsBinding = analyzeFindImportsSetup(node, mjsContent)
    if (importsBinding) {
      return importsBinding
    }
  }
  return paramNames.length > 0
    ? { __proto__: null, importsParam: paramNames[0], importsSetup: '' }
    : { __proto__: null, importsParam: 'info', importsSetup: 'var info={};' }
}

/**
 * Apply common transformations to MJS content.
 *
 * @param {object} options - Transform options.
 * @param {string} options.mtsContent - MJS content to transform.
 * @param {string} options.initFunctionName - Init function name.
 * @param {string} options.exportName - Export name.
 * @param {object} options.logger - Logger instance.
 *
 * @returns {Promise<string>} Transformed content
 */
export async function applyCommonTransforms(config) {
  const {
    exportName,
    initFunctionName,
    logger,
    mjsContent: inputContent,
  } = { __proto__: null, ...config } as typeof config

  let mjsContent = inputContent

  // Detect the Emscripten-generated module variable. Older onnx output
  // used `var D=Object.assign({},moduleArg)`; newer minified output
  // collapses this to `var n=moduleArg` and keeps moduleArg itself as
  // the module. The transform below replaces `return moduleRtn` with
  // `return <moduleVar>`, so we MUST read the real name instead of
  // hardcoding 'D' (which in newer builds is the HEAP8 typed-array
  // alias, causing the sync wrapper to export a 16MB Int8Array).
  const detectedModuleVar =
    mjsContent.match(
      /\bvar\s+([A-Za-z_$][\w$]*)\s*=\s*Object\.assign\(\s*\{\s*\}\s*,\s*moduleArg\s*\)/,
    )?.[1] ||
    mjsContent.match(/\bvar\s+([A-Za-z_$][\w$]*)\s*=\s*moduleArg\b/)?.[1]

  // === PASS 2: Main transformations ===
  const ast = Parser.parse(mjsContent, {
    ecmaVersion: 'latest',
    sourceType: 'module',
  })
  const s = new MagicString(mjsContent)

  // Helpers: Safe transformations for minified code (may have overlapping mods)
  const safeOverwrite = (start, end, content) => {
    try {
      s.overwrite(start, end, content)
    } catch {
      // Minified code - skip conflicting overwrites
    }
  }

  const safeRemove = (start, end) => {
    try {
      s.remove(start, end)
    } catch {
      // Minified code - skip conflicting removes, leave as dead code
    }
  }

  const topLevelStatementsToRemove = []
  const requireDeclaratorsToRemove = []
  const returnModuleToFix = []
  // Track functions that need WebAssembly.instantiate replacement
  const functionsToGut = []

  const sharedState = {
    detectedModuleVar,
    exportName,
    functionsToGut,
    initFunctionName,
    mjsContent,
    requireDeclaratorsToRemove,
    returnModuleToFix,
    safeOverwrite,
    safeRemove,
    topLevelStatementsToRemove,
  }

  ancestor(ast, {
    ...buildDeclVisitor(sharedState),
    ...buildCallVisitor(sharedState),
  })

  // Handle require declarators - remove them from their variable declarations
  // Loop variable is destructured.
  // oxlint-disable-next-line socket/prefer-cached-for-loop -- see above
  for (const { node: declaratorNode, varDecl } of requireDeclaratorsToRemove) {
    const declarators = varDecl.declarations
    const index = declarators.indexOf(declaratorNode)

    if (declarators.length === 1) {
      // Only one declarator - remove entire statement
      safeRemove(varDecl.start, varDecl.end)
    } else if (index === 0) {
      // First declarator in a list - remove it and the following comma
      const nextDeclarator = declarators[1]
      // Find comma between declarators
      const textBetween = s.original.slice(
        declaratorNode.end,
        nextDeclarator.start,
      )
      const commaIndex = textBetween.indexOf(',')
      if (commaIndex !== -1) {
        safeRemove(declaratorNode.start, declaratorNode.end + commaIndex + 1)
      } else {
        safeRemove(declaratorNode.start, nextDeclarator.start)
      }
    } else {
      // Not first declarator - remove from previous comma to this declarator
      const prevDeclarator = declarators[index - 1]
      const textBetween = s.original.slice(
        prevDeclarator.end,
        declaratorNode.start,
      )
      const commaIndex = textBetween.lastIndexOf(',')
      if (commaIndex !== -1) {
        safeRemove(prevDeclarator.end + commaIndex, declaratorNode.end)
      } else {
        safeRemove(prevDeclarator.end, declaratorNode.end)
      }
    }
  }

  // Remove all top-level statements containing await
  for (let i = 0, { length } = topLevelStatementsToRemove; i < length; i += 1) {
    const node = topLevelStatementsToRemove[i]
    safeRemove(node.start, node.end)
  }

  mjsContent = s.toString()

  // Second pass: Re-parse and fix 'return Module' statements to avoid split point conflicts
  // Always do this pass since safeOverwrite may have failed silently in minified code
  const ast2 = Parser.parse(mjsContent, {
    ecmaVersion: 'latest',
    sourceType: 'module',
  })
  const s2 = new MagicString(mjsContent)
  let foundReturnModule = false

  ancestor(ast2, {
    ReturnStatement(node, ancestors) {
      if (
        node.argument?.type === 'Identifier' &&
        node.argument?.name === 'Module'
      ) {
        // Walk up ancestors to find the function declaration
        for (let i = ancestors.length - 1; i >= 0; i--) {
          const frame = ancestors[i]
          if (frame.type === 'FunctionDeclaration') {
            // Check if we're inside the Module function
            if (frame.id?.name === 'Module') {
              s2.overwrite(node.argument.start, node.argument.end, 'moduleRtn')
              foundReturnModule = true
            }
            break
          }
        }
      }
    },
  })

  if (foundReturnModule) {
    mjsContent = s2.toString()
  }

  // Pass 3: Gut WebAssembly.instantiate functions with a fresh MagicString to avoid split-point conflicts.
  if (functionsToGut.length > 0) {
    const ast3 = Parser.parse(mjsContent, {
      ecmaVersion: 'latest',
      sourceType: 'module',
    })
    const s3 = new MagicString(mjsContent)
    let guttedCount = 0

    ancestor(ast3, {
      FunctionDeclaration(node) {
        const funcName = node.id?.name
        const {
          hasAsyncWebAssemblyCall,
          hasLoadingMechanism,
          isNotAlreadySync,
        } = analyzeInspectWasmFunction(node)

        const isNotMainFunction =
          funcName !== 'Module' &&
          funcName !== initFunctionName &&
          funcName !== exportName

        if (
          hasAsyncWebAssemblyCall &&
          hasLoadingMechanism &&
          isNotAlreadySync &&
          isNotMainFunction
        ) {
          // Extract param names
          const paramNames = node.params.map(param =>
            param.type === 'AssignmentPattern' ? param.left.name : param.name,
          )

          const { importsParam, importsSetup } = analyzeResolveImportsBinding(
            node,
            paramNames,
            mjsContent,
          )

          s3.overwrite(
            node.body.start + 1,
            node.body.end - 1,
            `\n  ${importsSetup}var module=new WebAssembly.Module(wasmBinary);var instance=new WebAssembly.Instance(module,${importsParam});return {instance:instance,module:module};\n`,
          )
          guttedCount++
          logger.substep(
            `Gutted WebAssembly.instantiate function: ${funcName || '(anonymous)'} (using imports: ${importsParam})`,
          )
        }
      },
    })

    if (guttedCount > 0) {
      mjsContent = s3.toString()
      logger.substep(
        `Gutted ${guttedCount} WebAssembly.instantiate function(s)`,
      )
    } else if (functionsToGut.length > 0) {
      logger.warn(
        `Expected to gut ${functionsToGut.length} function(s) but found ${guttedCount}`,
      )
    }
  }

  return mjsContent
}
