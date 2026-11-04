/*
 * wrap/tracker/ts_tracker_host.h - module host for the Polyend Tracker.
 *
 * The Tracker has no loader and no dlopen: this is the part that finds
 * .synthmodule files, maps each one into executable memory, applies its
 * relocations and hands the host the module's vtable.  It is the Tracker's
 * answer to logue/host/ts_host.c, with the operating system's file and
 * memory services behind a small platform table so the same code runs on a
 * bare-metal Cortex-M7 and in a host test.
 *
 * A module for the Tracker is a 32-bit little-endian ARM ELF, ET_DYN,
 * hard-float and Thumb (the M7 has no ARM state).  Address 0 is the image's
 * link base; the platform picks the load address and the host rebases.
 *
 * Provided by the platform:
 *   - directory iteration and whole-file reads (FatFS on the device, POSIX
 *     for the test),
 *   - executable memory (a reserved SDRAM window on the device),
 *   - a cache sync after relocations (small on the test, D-cache clean +
 *     I-cache invalidate on the device).
 *
 * MIT License.  Copyright (c) 2026 Gerard Braad.
 */
#ifndef TS_TRACKER_HOST_H
#define TS_TRACKER_HOST_H

#include <stdint.h>

#include "tinysynth.h"

#ifdef __cplusplus
extern "C" {
#endif

#define TS_TRACKER_MAX_MODULES 16
#define TS_TRACKER_PATH_LEN    128

/* The Tracker's audio geometry, fixed by the SDK. */
#define TS_TRACKER_SAMPLERATE  44100u
#define TS_TRACKER_FRAMES      128u

typedef struct ts_tracker_platform {
    /* Return a handle through *it, or 0 on failure.  dir_next returns the
     * next file name (no path), or NULL at the end.  The returned pointer is
     * only valid until the next call; the host copies it. */
    int         (*dir_open)(const char *dir, void **it);
    const char *(*dir_next)(void *it);
    void        (*dir_close)(void *it);

    /* A whole file, owned by the caller until file_free.  NULL on failure;
     * *size is then unspecified. */
    void *(*file_read)(const char *path, uint32_t *size);
    void  (*file_free)(void *buf);

    /* At least `size` bytes of writable, executable memory, zero-filled.
     * The device returns a slice of its reserved SDRAM; the test uses an
     * anonymous RWX mapping.  NULL on failure. */
    void *(*exec_alloc)(uint32_t size);
    void  (*exec_free)(void *p, uint32_t size);

    /* After the loader has written code and relocations. */
    void  (*cache_sync)(void *p, uint32_t size);

    /* 1 on the Tracker: refuse a module whose entry point is not Thumb code
     * (st_value bit 0 clear).  The host test loads armhf ARM-state modules,
     * so it leaves this at 0. */
    int require_thumb;
} ts_tracker_platform_t;

typedef struct ts_tracker_entry {
    char        path[TS_TRACKER_PATH_LEN];
    const char *name;       /* the module's own name, in its image */
    uint8_t     params;
    uint8_t     voices;
} ts_tracker_entry_t;

/* Scan `dir` for .synthmodule files and load the ones this host accepts.
 * Files that fail to load are skipped; the reason is in ts_tracker_host_error()
 * when the count is lower than the number of files seen.  Returns the number
 * of usable modules, or -1 if the directory could not be opened. */
int ts_tracker_host_scan(const ts_tracker_platform_t *platform, const char *dir);

int ts_tracker_host_count(void);
const ts_tracker_entry_t *ts_tracker_host_entry(int index);

/* Make entry `index` the active module.  Returns 1 on success. */
int ts_tracker_host_select(int index);
const ts_module_t *ts_tracker_host_module(void);
int ts_tracker_host_selected(void);   /* -1 when nothing is selected */

/* A calloc'd, initialised instance of the active module, or NULL.  The
 * caller owns it.  The audio rate and block size are the Tracker's. */
void *ts_tracker_host_instance_new(void);
void ts_tracker_host_instance_free(void *instance);

void ts_tracker_host_deselect(void);

const char *ts_tracker_host_error(void);

/* Used by the ELF loader; not part of the host's interface. */
void ts_tracker_error_set(const char *msg);

#ifdef __cplusplus
}
#endif

#endif /* TS_TRACKER_HOST_H */
