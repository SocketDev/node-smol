// ============================================================================
// binject_internal.h — Cross-unit helpers shared by the binject_*.c files
// ============================================================================
//
// WHAT THIS FILE DOES
// Declares the small helpers that binject.c, binject_stub_cache.c, and
// binject_commands.c all need. It is NOT part of binject's public surface —
// nothing outside src/socketsecurity/binject/ should include it. The public
// API lives in binject.h.
//
// WHY IT EXISTS
// binject.c was split along its three phases (core primitives, compressed-stub
// cache handling, CLI command orchestration). The stat wrapper below used to be
// a file-local `static` shared by all three; once they became separate
// translation units it had to be promoted to a single internal definition
// (in binject.c) declared here, instead of being copy-pasted three times.
// ============================================================================

#ifndef BINJECT_INTERNAL_H
#define BINJECT_INTERNAL_H

#include <sys/stat.h>

/* Cross-platform stat wrapper — avoids #define stat _stat which breaks
 * struct stat declarations on Windows. Defined in binject.c. */
int binject_stat(const char *path, struct stat *st);

#define BINJECT_STAT_STRUCT struct stat

#endif /* BINJECT_INTERNAL_H */
