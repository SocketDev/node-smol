// ============================================================================
// node_resolve.h — Finding a usable host Node.js binary
// ============================================================================
//
// WHAT THIS FILE DOES
// Declares the two entry points binject uses to locate a Node.js executable on
// the machine running the build: one that prefers an exact version match, and a
// convenience wrapper for "any working node will do".
//
// WHY IT EXISTS
// binject shells out to `node --experimental-sea-config` to produce a SEA blob,
// and a SEA blob is version-specific — a blob built by Node 24 will not load in
// Node 25. Picking the right node is therefore a correctness concern, not a
// convenience, so the search (env override, $PATH, then every version manager
// and CI image layout we know about) lives in its own translation unit.
//
// This is an INTERNAL header: it carries what used to be `static` helpers in
// main.c, promoted to `binject_`-prefixed symbols when main.c was split along
// its three concerns (CLI dispatch, node discovery, SEA blob generation).
// Nothing outside src/socketsecurity/binject/ should include it.
// ============================================================================

#ifndef BINJECT_NODE_RESOLVE_H
#define BINJECT_NODE_RESOLVE_H

/**
 * Find Node.js binary matching the expected version.
 *
 * Search order:
 * 1. BINJECT_NODE_PATH env var - explicit override, skips all auto-detection
 * 2. $PATH - prefer user's environment (respects nvm use, volta, fnm, etc.)
 * 3. Version manager paths with specific version (nvm, fnm, volta, asdf,
 *    nodenv, n, mise, plus the Windows and CI/CD image layouts)
 * 4. Environment variables: NVM_BIN, FNM_MULTISHELL_PATH, VOLTA_HOME
 *
 * If expected_version is provided, checks each candidate's version.
 * If no match found, falls back to first available node.
 *
 * @param expected_version Expected Node.js version (e.g., "25.5.0"), or NULL
 * @param found_version_out If non-NULL, receives the found version (caller must free)
 * @param is_match_out If non-NULL, set to 1 if version matched, 0 otherwise
 * @return Path to node binary (caller must free), or NULL if not found
 */
char* binject_find_matching_node_binary(const char *expected_version,
                                        char **found_version_out,
                                        int *is_match_out);

/**
 * Find system Node.js binary for running --experimental-sea-config
 * Returns path to node binary, or NULL if not found
 * Caller is responsible for freeing the returned string
 *
 * Note: This is a convenience wrapper around binject_find_matching_node_binary()
 * when no version matching is needed.
 */
char* binject_find_system_node_binary(void);

#endif /* BINJECT_NODE_RESOLVE_H */
