/*
 * wrap/tracker/ts_tracker_elf.c - ELF32 ARM image loader for the Tracker.
 *
 * The parsing, checks and R_ARM_RELATIVE handling are the ones the drumlogue
 * host already runs (logue/host/ts_elf.c); what changes is where the memory
 * comes from and how the instruction cache is made to see the result.  Both
 * are platform callbacks, so the same loader runs under the host test and on
 * the bare-metal Cortex-M7.
 *
 * A .synthmodule for the Tracker is built -fPIC -shared -nostdlib -lgcc:
 * its only dynamic relocations are R_ARM_RELATIVE, everything else is bound
 * at link time.  A module that needs more than that is refused by name.
 *
 * MIT License.  Copyright (c) 2026 Gerard Braad.
 */
#include "ts_tracker_elf.h"

#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------- ELF types -- */

typedef struct {
    uint8_t  e_ident[16];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint32_t e_entry;
    uint32_t e_phoff;
    uint32_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
} th_ehdr_t;

typedef struct {
    uint32_t p_type;
    uint32_t p_offset;
    uint32_t p_vaddr;
    uint32_t p_paddr;
    uint32_t p_filesz;
    uint32_t p_memsz;
    uint32_t p_flags;
    uint32_t p_align;
} th_phdr_t;

typedef struct {
    uint32_t d_tag;
    uint32_t d_val;
} th_dyn_t;

typedef struct {
    uint32_t r_offset;
    uint32_t r_info;
} th_rel_t;

typedef struct {
    uint32_t st_name;
    uint32_t st_value;
    uint32_t st_size;
    uint8_t  st_info;
    uint8_t  st_other;
    uint16_t st_shndx;
} th_sym_t;

typedef char th_ehdr_size_check[(sizeof(th_ehdr_t) == 52) ? 1 : -1];
typedef char th_phdr_size_check[(sizeof(th_phdr_t) == 32) ? 1 : -1];
typedef char th_dyn_size_check[(sizeof(th_dyn_t) == 8) ? 1 : -1];
typedef char th_rel_size_check[(sizeof(th_rel_t) == 8) ? 1 : -1];
typedef char th_sym_size_check[(sizeof(th_sym_t) == 16) ? 1 : -1];

enum {
    TH_ET_DYN = 3,
    TH_EM_ARM = 40,
    TH_PT_LOAD = 1,
    TH_PT_DYNAMIC = 2,

    TH_DT_NULL = 0,
    TH_DT_HASH = 4,
    TH_DT_STRTAB = 5,
    TH_DT_SYMTAB = 6,
    TH_DT_STRSZ = 10,
    TH_DT_SYMENT = 11,
    TH_DT_REL = 17,
    TH_DT_RELSZ = 18,
    TH_DT_RELENT = 19,
    TH_DT_GNU_HASH = 0x6ffffef5u,

    TH_R_ARM_NONE = 0,
    TH_R_ARM_RELATIVE = 23,

    /* EF_ARM_ABI_FLOAT_HARD: the module's float ABI has to match the host's,
     * or a float argument travels in the wrong registers. */
    TH_EF_ARM_ABI_FLOAT_HARD = 0x00000400u
};

#define TH_MAX_FILE_BYTES (4u * 1024u * 1024u)
#define TH_MAX_SYMS       65536u

struct ts_tracker_elf {
    const ts_tracker_platform_t *platform;
    void       *map;
    uint32_t    map_size;
    uint32_t    bias;           /* address of vaddr 0 */
    const th_sym_t *symtab;
    uint32_t    nsyms;
    const char *strtab;
    uint32_t    strsz;
    const uint32_t *bucket;
    uint32_t    nbucket;
    const uint32_t *chain;
    const uint32_t *gnu;
};

/* ------------------------------------------------------------- symbols --- */

static uint32_t gnu_hash(const char *name) {
    uint32_t h = 5381u;
    const uint8_t *p = (const uint8_t *)name;

    while (*p) h = (h << 5) + h + *p++;
    return h;
}

static const th_sym_t *find_sym(const ts_tracker_elf_t *img, const char *name) {
    uint32_t idx;

    if (img->gnu) {
        const uint32_t *hdr = img->gnu;
        uint32_t nbuckets = hdr[0];
        uint32_t symoffset = hdr[1];
        uint32_t bloom_size = hdr[2];
        uint32_t bloom_shift = hdr[3];
        const uint32_t *bloom = hdr + 4;
        const uint32_t *buckets = bloom + bloom_size;
        const uint32_t *chain = buckets + nbuckets;
        uint32_t h, word, mask;

        if (nbuckets == 0u || bloom_size == 0u) return NULL;
        h = gnu_hash(name);
        word = bloom[(h / 32u) % bloom_size];
        mask = (1u << (h % 32u)) | (1u << ((h >> bloom_shift) % 32u));
        if ((word & mask) != mask) return NULL;

        idx = buckets[h % nbuckets];
        if (idx < symoffset) return NULL;
        while (idx < img->nsyms) {
            uint32_t ch = chain[idx - symoffset];
            if ((ch | 1u) == (h | 1u)) {
                const th_sym_t *s = &img->symtab[idx];
                if (s->st_name < img->strsz &&
                    strcmp(img->strtab + s->st_name, name) == 0)
                    return s;
            }
            if (ch & 1u) break;
            idx++;
        }
        return NULL;
    }

    if (img->bucket && img->chain) {
        const char *p;
        uint32_t h = 0u, i;

        for (p = name; *p; p++) {
            uint32_t g;
            h = (h << 4) + (uint8_t)*p;
            g = h & 0xf0000000u;
            if (g) h ^= g >> 24;
            h &= ~g;
        }
        for (i = img->bucket[h % img->nbucket]; i != 0u && i < img->nsyms;
             i = img->chain[i]) {
            const th_sym_t *s = &img->symtab[i];
            if (s->st_name < img->strsz &&
                strcmp(img->strtab + s->st_name, name) == 0)
                return s;
        }
    }
    return NULL;
}

/* ----------------------------------------------------------- relocation -- */

static int relocate(ts_tracker_elf_t *img, uint32_t rel_vaddr,
                    uint32_t rel_size) {
    const th_rel_t *rel;
    uint32_t i, count;

    if (rel_size == 0u) return 1;
    if (rel_vaddr == 0u || (rel_size % sizeof(th_rel_t)) != 0u) {
        ts_tracker_error_set("bad relocation table");
        return 0;
    }
    rel = (const th_rel_t *)(uintptr_t)(img->bias + rel_vaddr);
    count = rel_size / (uint32_t)sizeof(th_rel_t);

    for (i = 0; i < count; i++) {
        uint32_t type = rel[i].r_info & 0xffu;

        if (type == TH_R_ARM_NONE) continue;
        if (type != TH_R_ARM_RELATIVE) {
            ts_tracker_error_set("module needs a relocation this loader cannot apply");
            return 0;
        }
        {
            uint32_t *slot = (uint32_t *)(uintptr_t)(img->bias + rel[i].r_offset);
            *slot += img->bias;
        }
    }
    return 1;
}

/* ----------------------------------------------------------------- load -- */

static void image_free(ts_tracker_elf_t *img) {
    if (!img) return;
    if (img->map && img->platform)
        img->platform->exec_free(img->map, img->map_size);
    free(img);
}

ts_tracker_elf_t *ts_tracker_elf_load(const ts_tracker_platform_t *platform,
                                      const void *file, uint32_t len) {
    const th_ehdr_t *eh = (const th_ehdr_t *)file;
    const th_phdr_t *ph;
    ts_tracker_elf_t *img;
    uint32_t min_vaddr = 0xffffffffu, max_end = 0u;
    uint32_t dyn_vaddr = 0u, dyn_size = 0u;
    uint32_t i, map_size;
    int loads = 0;

    if (!platform || !file) {
        ts_tracker_error_set("no module data");
        return NULL;
    }
    if (len < sizeof(th_ehdr_t) || memcmp(eh->e_ident, "\177ELF", 4) != 0) {
        ts_tracker_error_set("not an ELF file");
        return NULL;
    }
    if (eh->e_ident[4] != 1 || eh->e_ident[5] != 1) {
        ts_tracker_error_set("not a 32-bit little-endian object");
        return NULL;
    }
    if (eh->e_machine != TH_EM_ARM) {
        ts_tracker_error_set("not an ARM object");
        return NULL;
    }
    if (eh->e_type != TH_ET_DYN) {
        ts_tracker_error_set("not a position-independent object");
        return NULL;
    }
    if ((eh->e_flags & TH_EF_ARM_ABI_FLOAT_HARD) == 0u) {
        ts_tracker_error_set("not a hard-float object");
        return NULL;
    }
    if (eh->e_phentsize != sizeof(th_phdr_t) || eh->e_phnum == 0u ||
        eh->e_phoff > len ||
        (uint32_t)eh->e_phnum * sizeof(th_phdr_t) > len - eh->e_phoff) {
        ts_tracker_error_set("bad program headers");
        return NULL;
    }

    ph = (const th_phdr_t *)(const void *)((const uint8_t *)file + eh->e_phoff);
    for (i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type == TH_PT_LOAD) {
            uint32_t end;
            if (ph[i].p_filesz > ph[i].p_memsz ||
                ph[i].p_offset > len ||
                ph[i].p_filesz > len - ph[i].p_offset) {
                ts_tracker_error_set("segment reaches past the file");
                return NULL;
            }
            end = ph[i].p_vaddr + ph[i].p_memsz;
            if (end < ph[i].p_vaddr) {
                ts_tracker_error_set("segment wraps the address space");
                return NULL;
            }
            if (ph[i].p_vaddr < min_vaddr) min_vaddr = ph[i].p_vaddr;
            if (end > max_end) max_end = end;
            loads++;
        } else if (ph[i].p_type == TH_PT_DYNAMIC) {
            dyn_vaddr = ph[i].p_vaddr;
            dyn_size = ph[i].p_memsz;
        }
    }
    if (loads == 0 || min_vaddr == 0xffffffffu || max_end <= min_vaddr) {
        ts_tracker_error_set("no loadable segments");
        return NULL;
    }
    if (dyn_vaddr == 0u || dyn_size < sizeof(th_dyn_t)) {
        ts_tracker_error_set("no dynamic section");
        return NULL;
    }

    map_size = max_end - min_vaddr;
    img = (ts_tracker_elf_t *)calloc(1, sizeof *img);
    if (!img) {
        ts_tracker_error_set("out of memory");
        return NULL;
    }
    img->platform = platform;
    img->map = platform->exec_alloc(map_size);
    if (!img->map) {
        free(img);
        ts_tracker_error_set("no executable memory for the module");
        return NULL;
    }
    img->map_size = map_size;
    img->bias = (uint32_t)(uintptr_t)img->map - min_vaddr;

    /* The platform promises zero-filled memory, but a bump arena that was
     * reused does not have to: clear it, so a segment gap or BSS cannot read
     * a previous module's state. */
    memset(img->map, 0, map_size);

    for (i = 0; i < eh->e_phnum; i++) {
        uint8_t *dst;
        if (ph[i].p_type != TH_PT_LOAD) continue;
        dst = (uint8_t *)(uintptr_t)(img->bias + ph[i].p_vaddr);
        if (ph[i].p_filesz)
            memcpy(dst, (const uint8_t *)file + ph[i].p_offset, ph[i].p_filesz);
        if (ph[i].p_memsz > ph[i].p_filesz)
            memset(dst + ph[i].p_filesz, 0, ph[i].p_memsz - ph[i].p_filesz);
    }

    {
        const th_dyn_t *dyn =
            (const th_dyn_t *)(uintptr_t)(img->bias + dyn_vaddr);
        uint32_t n = dyn_size / (uint32_t)sizeof(th_dyn_t);
        uint32_t rel = 0u, relsz = 0u;
        uint32_t hash = 0u, gnu = 0u, symtab = 0u, strtab = 0u;

        for (i = 0; i < n && dyn[i].d_tag != TH_DT_NULL; i++) {
            switch (dyn[i].d_tag) {
            case TH_DT_REL:      rel = dyn[i].d_val; break;
            case TH_DT_RELSZ:    relsz = dyn[i].d_val; break;
            case TH_DT_HASH:     hash = dyn[i].d_val; break;
            case TH_DT_GNU_HASH: gnu = dyn[i].d_val; break;
            case TH_DT_SYMTAB:   symtab = dyn[i].d_val; break;
            case TH_DT_STRTAB:   strtab = dyn[i].d_val; break;
            case TH_DT_STRSZ:    img->strsz = dyn[i].d_val; break;
            default: break;
            }
        }

        if (!relocate(img, rel, relsz)) {
            image_free(img);
            return NULL;
        }

        if (symtab == 0u || strtab == 0u || img->strsz == 0u) {
            ts_tracker_error_set("module has no dynamic symbol table");
            image_free(img);
            return NULL;
        }
        img->symtab = (const th_sym_t *)(uintptr_t)(img->bias + symtab);
        img->strtab = (const char *)(uintptr_t)(img->bias + strtab);

        if (hash != 0u) {
            const uint32_t *h = (const uint32_t *)(uintptr_t)(img->bias + hash);
            img->nbucket = h[0];
            img->nsyms = h[1];
            img->bucket = h + 2;
            img->chain = img->bucket + img->nbucket;
        } else if (gnu != 0u) {
            const uint32_t *h = (const uint32_t *)(uintptr_t)(img->bias + gnu);
            uint32_t nbuckets = h[0], symoffset = h[1], bloom_size = h[2];
            const uint32_t *buckets = h + 4 + bloom_size;
            const uint32_t *chain = buckets + nbuckets;
            uint32_t b, last = symoffset;

            if (nbuckets == 0u || bloom_size == 0u) {
                ts_tracker_error_set("empty GNU hash table");
                image_free(img);
                return NULL;
            }
            for (b = 0; b < nbuckets; b++) {
                uint32_t idx = buckets[b];
                uint32_t steps = 0;

                if (idx == 0u) continue;
                if (idx < symoffset) {
                    ts_tracker_error_set("bad GNU hash bucket");
                    image_free(img);
                    return NULL;
                }
                for (;;) {
                    uint32_t ch;
                    if (steps++ > TH_MAX_SYMS) {
                        ts_tracker_error_set("bad GNU hash chain");
                        image_free(img);
                        return NULL;
                    }
                    ch = chain[idx - symoffset];
                    idx++;
                    if (ch & 1u) break;
                }
                if (idx > last) last = idx;
            }
            img->gnu = h;
            img->nsyms = last;
        } else {
            ts_tracker_error_set("module has no symbol hash table");
            image_free(img);
            return NULL;
        }
    }

    /* The relocations were written as data; make instruction fetch see the
     * image the loader just wrote. */
    if (platform->cache_sync)
        platform->cache_sync(img->map, img->map_size);
    return img;
}

void *ts_tracker_elf_sym(const ts_tracker_elf_t *img, const char *name) {
    const th_sym_t *s;

    if (!img || !name) return NULL;
    s = find_sym(img, name);
    if (!s || s->st_shndx == 0u) return NULL;
    /* The Thumb bit is part of a function symbol's value on ARM: keep it.
     * A bit-0-clear value is ARM-state code, which the M7 cannot run -- the
     * host's require_thumb check rejects it. */
    return (void *)(uintptr_t)(img->bias + s->st_value);
}

void ts_tracker_elf_close(ts_tracker_elf_t *img) {
    image_free(img);
}
