/*
 * wrap/tracker/ts_tracker_host.c - module host for the Polyend Tracker.
 *
 * Scans a directory for .synthmodule files, loads each one through the
 * platform's executable memory, and keeps the module vtables.  This is the
 * Tracker's counterpart of logue/host/ts_host.c, minus Linux.
 *
 * Nothing here runs on the audio thread: the firmware scans once at boot and
 * creates an instance per Tracker voice from the active module.
 *
 * MIT License.  Copyright (c) 2026 Gerard Braad.
 */
#include "ts_tracker_host.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ts_tracker_elf.h"

static const ts_tracker_platform_t *s_platform;
static ts_tracker_elf_t            *s_images[TS_TRACKER_MAX_MODULES];
static ts_tracker_entry_t           s_entries[TS_TRACKER_MAX_MODULES];
static int                          s_count;
static int                          s_active = -1;
static char                         s_err[96] = "no error";

void ts_tracker_error_set(const char *msg) {
    snprintf(s_err, sizeof s_err, "%s", msg ? msg : "error");
}

const char *ts_tracker_host_error(void) {
    return s_err;
}

static int has_suffix(const char *name, const char *suffix) {
    size_t n = strlen(name), s = strlen(suffix);

    return n >= s && strcmp(name + n - s, suffix) == 0;
}

static void reset(void) {
    int i;

    for (i = 0; i < s_count; i++) {
        ts_tracker_elf_close(s_images[i]);
        s_images[i] = NULL;
    }
    memset(s_entries, 0, sizeof s_entries);
    s_count = 0;
    s_active = -1;
}

int ts_tracker_host_scan(const ts_tracker_platform_t *platform, const char *dir) {
    void *it;
    const char *name;
    int skipped = 0;

    if (!platform || !platform->dir_open || !dir) {
        ts_tracker_error_set("no module platform");
        return -1;
    }

    reset();
    s_platform = platform;
    s_err[0] = '\0';

    if (!platform->dir_open(dir, &it)) {
        ts_tracker_error_set("cannot open the module directory");
        return -1;
    }

    while ((name = platform->dir_next(it)) != NULL) {
        char path[TS_TRACKER_PATH_LEN];
        const ts_module_t *(*entry)(void);
        const ts_module_t *m;
        ts_tracker_elf_t *img;
        void *file;
        uint32_t size;

        if (!has_suffix(name, ".synthmodule"))
            continue;

        if (snprintf(path, sizeof path, "%s/%s", dir, name) >= (int)sizeof path) {
            ts_tracker_error_set("module path is too long");
            skipped++;
            continue;
        }
        if (s_count >= TS_TRACKER_MAX_MODULES) {
            ts_tracker_error_set("too many modules");
            skipped++;
            continue;
        }

        file = platform->file_read(path, &size);
        if (!file) {
            ts_tracker_error_set("cannot read a module file");
            skipped++;
            continue;
        }
        img = ts_tracker_elf_load(platform, file, size);
        platform->file_free(file);
        if (!img) {
            skipped++;
            continue;
        }

        entry = (const ts_module_t *(*)(void))ts_tracker_elf_sym(img, "ts_module");
        if (!entry) {
            ts_tracker_error_set("module has no ts_module symbol");
            ts_tracker_elf_close(img);
            skipped++;
            continue;
        }
        if (platform->require_thumb && (((uintptr_t)entry & 1u) == 0u)) {
            ts_tracker_error_set("not Thumb code (the M7 cannot run ARM state)");
            ts_tracker_elf_close(img);
            skipped++;
            continue;
        }

        m = entry();
        if (!m || !TS_ABI_OK(m->abi_version) ||
            m->struct_size < TS_MODULE_BASE_BYTES ||
            m->num_params > TS_MAX_PARAMS ||
            m->instance_size > TS_MAX_INSTANCE_BYTES) {
            ts_tracker_error_set("module does not match the tinysynth ABI");
            ts_tracker_elf_close(img);
            skipped++;
            continue;
        }

        snprintf(s_entries[s_count].path, sizeof s_entries[s_count].path,
                 "%s", path);
        s_entries[s_count].name = m->name;
        s_entries[s_count].params = m->num_params;
        s_entries[s_count].voices = m->max_voices;
        s_images[s_count] = img;
        s_count++;
    }

    platform->dir_close(it);

    if (skipped && s_count == 0 && s_err[0] == '\0')
        ts_tracker_error_set("no usable module in the directory");
    return s_count;
}

int ts_tracker_host_count(void) {
    return s_count;
}

const ts_tracker_entry_t *ts_tracker_host_entry(int index) {
    if (index < 0 || index >= s_count) return NULL;
    return &s_entries[index];
}

int ts_tracker_host_select(int index) {
    if (index < 0 || index >= s_count) {
        ts_tracker_error_set("no such module");
        return 0;
    }
    s_active = index;
    return 1;
}

int ts_tracker_host_selected(void) {
    return s_active;
}

const ts_module_t *ts_tracker_host_module(void) {
    if (s_active < 0 || s_active >= s_count) return NULL;
    {
        const ts_module_t *(*entry)(void) =
            (const ts_module_t *(*)(void))ts_tracker_elf_sym(
                s_images[s_active], "ts_module");
        return entry ? entry() : NULL;
    }
}

void *ts_tracker_host_instance_new(void) {
    const ts_module_t *m = ts_tracker_host_module();
    ts_desc_t desc;
    void *inst;
    uint8_t i;

    if (!m || !m->init) {
        ts_tracker_error_set("no module selected");
        return NULL;
    }
    inst = calloc(1, m->instance_size ? m->instance_size : 1u);
    if (!inst) {
        ts_tracker_error_set("out of memory for the instance");
        return NULL;
    }

    desc.samplerate = TS_TRACKER_SAMPLERATE;
    desc.max_frames = TS_TRACKER_FRAMES;
    desc.input_channels = 0;
    desc.output_channels = 1;

    if (m->init(inst, &desc) != 0) {
        free(inst);
        ts_tracker_error_set("the module refused its instance");
        return NULL;
    }
    for (i = 0; i < m->num_params; i++)
        m->set_param(inst, i, m->params[i].init);
    return inst;
}

void ts_tracker_host_instance_free(void *instance) {
    free(instance);
}

void ts_tracker_host_deselect(void) {
    s_active = -1;
}
