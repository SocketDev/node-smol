// ============================================================================
// sea_blob.h — Generating a SEA blob from a sea-config.json
// ============================================================================
//
// WHAT THIS FILE DOES
// Declares the single entry point that turns a sea-config.json into a SEA blob
// file on disk by running `node --experimental-sea-config` for you.
//
// WHY IT EXISTS
// Both `binject inject --sea config.json` and `binject blob config.json` need
// the same multi-step dance: find a version-appropriate node, warn when the
// version or platform cannot produce usable code cache, run node, then work out
// where node wrote the blob. Keeping that in one unit stops main.c from owning
// it twice.
//
// This is an INTERNAL header: it carries what used to be a `static` helper in
// main.c, promoted to a `binject_`-prefixed symbol when main.c was split along
// its three concerns (CLI dispatch, node discovery, SEA blob generation).
// Nothing outside src/socketsecurity/binject/ should include it.
// ============================================================================

#ifndef BINJECT_SEA_BLOB_H
#define BINJECT_SEA_BLOB_H

/**
 * Generate SEA blob from JSON config using node --experimental-sea-config
 * Uses the target executable (node-smol) to generate the blob, ensuring
 * the blob is created with the same Node.js version that will run it.
 * Returns path to generated blob (caller must free), or NULL on error
 */
char* binject_generate_sea_blob_from_config(const char *config_path,
                                            const char *executable);

#endif /* BINJECT_SEA_BLOB_H */
