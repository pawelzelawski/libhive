/*
 * hive_internal.h — internal shared types and forward declarations
 *
 * Not included by embedders. Internal to the library only.
 * See ARCHITECTURE.md §2 for the session struct layout.
 * See CODING_STANDARDS.md §1.3 for the _Static_assert policy.
 */

#ifndef HIVE_INTERNAL_H
#define HIVE_INTERNAL_H

#include <stddef.h>
#include <stdint.h>

#include "../include/hive.h"

/* ------------------------------------------------------------------ */
/* ASan helpers (HIVE_DEBUG builds only)                               */
/* See TECH_STACK.md §7.2 and CODING_STANDARDS.md §3.5.               */
/* ------------------------------------------------------------------ */

#if defined(HIVE_DEBUG) && defined(__SANITIZE_ADDRESS__)
#include <sanitizer/asan_interface.h>
#define HIVE_ASAN_POISON(ptr, size) __asan_poison_memory_region((ptr), (size))
#define HIVE_ASAN_UNPOISON(ptr, size)                                          \
	__asan_unpoison_memory_region((ptr), (size))
#else
#define HIVE_ASAN_POISON(ptr, size) ((void)0)
#define HIVE_ASAN_UNPOISON(ptr, size) ((void)0)
#endif

/* ------------------------------------------------------------------ */
/* Compile-time struct size checks                                     */
/* These are placeholders in Phase 1. Real assertions activate as     */
/* struct definitions land in Phase 2 onward.                         */
/* See CODING_STANDARDS.md §1.3.                                      */
/* ------------------------------------------------------------------ */

_Static_assert(1 == 1, "placeholder — hive_stream_t size check (Phase 2+)");
_Static_assert(1 == 1,
               "placeholder — stream_hash_entry_t size check (Phase 2+)");
_Static_assert(1 == 1, "placeholder — hpack_entry_t size check (Phase 3+)");
_Static_assert(1 == 1, "placeholder — hive_settings_t size check (Phase 4+)");
/* huff_entry_t size check is active in src/hive_hpack.h next to the type. */

#endif /* HIVE_INTERNAL_H */
