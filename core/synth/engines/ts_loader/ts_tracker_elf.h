/*
 * wrap/tracker/ts_tracker_elf.h - ELF32 ARM image loader (internal).
 *
 * Split out of the host so the parsing and relocation can be read and tested
 * on its own.  It uses only the platform's exec_alloc/exec_free/cache_sync.
 *
 * MIT License.  Copyright (c) 2026 Gerard Braad.
 */
#ifndef TS_TRACKER_ELF_H
#define TS_TRACKER_ELF_H

#include <stdint.h>

#include "ts_tracker_host.h"

typedef struct ts_tracker_elf ts_tracker_elf_t;

/* Map `file` into executable memory and apply its relocations.  The bytes
 * are not retained.  NULL on failure, with ts_tracker_host_error() set. */
ts_tracker_elf_t *ts_tracker_elf_load(const ts_tracker_platform_t *platform,
                                      const void *file, uint32_t size);

/* Address of a defined symbol, Thumb bit intact (call it as a function
 * pointer).  NULL when the symbol is missing or undefined. */
void *ts_tracker_elf_sym(const ts_tracker_elf_t *img, const char *name);

void ts_tracker_elf_close(ts_tracker_elf_t *img);

#endif /* TS_TRACKER_ELF_H */
