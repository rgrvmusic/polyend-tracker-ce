/*
 * wrap/tracker/platform_tracker.cpp - the Polyend Tracker+'s platform.
 *
 * The firmware side of the same four things platform_posix.c provides to the
 * test: list a folder, read a file, hand out executable memory, and make the
 * written code visible to instruction fetch.  The folder and file calls go
 * through the firmware's own FileSystem interface, so FatFS, the SD hot-plug
 * bookkeeping and the file pools stay where the firmware put them.
 *
 * The module image lives in external SDRAM, which the linker scripts mark
 * read-write-execute and which no MPU setup restricts.  It is a static
 * __NOINIT(EXTERNAL_RAM) arena rather than a malloc: the size is known, the
 * firmware already parks buffers there, and nothing has to survive a reset.
 *
 * MIT License.  Copyright (c) 2026 Gerard Braad.
 */
#include "platform_tracker.h"

/* core/synth/ is compiled for both products; only the IMX firmware has the
 * synth feature and the external SDRAM this platform parks module images in. */
#ifdef FEATURE_SYNTH

#include <cstdlib>
#include <cstring>

#include "Platform.h"
#include "Tracker.h"
#include "cr_section_macros.h"

namespace
{

/* Sixteen modules of at most a few dozen KiB each; 1 MiB leaves headroom for
 * a module with big wavetables.  The arena is bumped and never freed: the
 * host scans once at boot (see the loader engine), so a second scan would
 * have to restart the device anyway. */
constexpr uint32_t kArenaBytes = 1u << 20;

constexpr int kMaxNames = TS_TRACKER_MAX_MODULES * 2;
constexpr int kMaxNameLen = 63;

__NOINIT(EXTERNAL_RAM) uint8_t s_arena[kArenaBytes];
uint32_t s_arena_used;

struct DirIter
{
    int count;
    int next;
    char names[kMaxNames * (kMaxNameLen + 1)];
    char* list[kMaxNames];
};

FileSystem* fileSystem()
{
    Tracker* tracker = Tracker::getInstance();
    if (tracker == nullptr)
    {
        return nullptr;
    }
    Platform* platform = tracker->getPlatform();
    return platform != nullptr ? platform->getFileSystem() : nullptr;
}

bool moduleName(const char* name)
{
    static constexpr char kSuffix[] = ".synthmodule";
    const std::size_t n = std::strlen(name);
    const std::size_t s = sizeof kSuffix - 1u;

    return n > s && std::memcmp(name + n - s, kSuffix, s) == 0;
}

int dirOpen(const char* dir, void** it)
{
    FileSystem* fs = fileSystem();
    if (fs == nullptr || dir == nullptr)
    {
        return 0;
    }

    /* The firmware's list call fills a flat name buffer and a pointer table
     * pointing into it; the iterator owns both for as long as the scan. */
    DirIter* p = static_cast<DirIter*>(std::calloc(1, sizeof *p));
    if (p == nullptr)
    {
        return 0;
    }
    p->count = kMaxNames;
    fs->listDirContents(dir,
                        FileSystem::DirContentType::kFiles,
                        [](const char* name) { return moduleName(name); },
                        p->names,
                        static_cast<int>(sizeof p->names),
                        kMaxNameLen,
                        p->count,
                        p->list);
    *it = p;
    return 1;
}

const char* dirNext(void* it)
{
    DirIter* p = static_cast<DirIter*>(it);

    return p != nullptr && p->next < p->count ? p->list[p->next++] : nullptr;
}

void dirClose(void* it)
{
    std::free(it);
}

void* fileRead(const char* path, uint32_t* size)
{
    FileSystem* fs = fileSystem();
    if (fs == nullptr || path == nullptr || size == nullptr)
    {
        return nullptr;
    }

    const std::size_t len = fs->getFileSize(path);
    if (len == 0u || len > (4u * 1024u * 1024u))
    {
        return nullptr;
    }

    auto [opened, handle] = fs->openFile(path, FileSystem::FileOpenMode::kRead);
    if (!opened || handle == nullptr)
    {
        return nullptr;
    }

    void* buf = std::malloc(len);
    if (buf == nullptr)
    {
        fs->closeFile(handle);
        return nullptr;
    }
    const std::size_t got = fs->readFile(buf, len, handle);
    fs->closeFile(handle);
    if (got != len)
    {
        std::free(buf);
        return nullptr;
    }
    *size = static_cast<uint32_t>(len);
    return buf;
}

void fileFree(void* buf)
{
    std::free(buf);
}

void* execAlloc(uint32_t size)
{
    const uint32_t at = (s_arena_used + 31u) & ~31u;

    if (size == 0u || at > kArenaBytes || size > kArenaBytes - at)
    {
        return nullptr;
    }
    s_arena_used = at + size;
    return s_arena + at;
}

/* The host only frees images when it starts a new scan, which the loader
 * engine does not do while it is running. */
void execFree(void* /*p*/, uint32_t /*size*/)
{
}

/* The relocations and the segment copies were written through the data cache,
 * and the same lines are about to be fetched as instructions.  Clean D-cache
 * to the point of coherency, then invalidate the I-cache line by line.  CMSIS
 * spells these SCB_DCCMVAC and SCB_ICIMVAU; core_cm7.h is not on the core
 * build's include path, and two register writes do not need it. */
void cacheSync(void* p, uint32_t size)
{
#if defined(__arm__)
    const uintptr_t base = reinterpret_cast<uintptr_t>(p) & ~static_cast<uintptr_t>(31u);
    const uintptr_t end =
      (reinterpret_cast<uintptr_t>(p) + size + 31u) & ~static_cast<uintptr_t>(31u);

    for (uintptr_t a = base; a < end; a += 32u)
    {
        *reinterpret_cast<volatile uint32_t*>(0xE000EF68u) = static_cast<uint32_t>(a);
    }
    __asm volatile("dsb 0xf" ::: "memory");

    for (uintptr_t a = base; a < end; a += 32u)
    {
        *reinterpret_cast<volatile uint32_t*>(0xE000EF58u) = static_cast<uint32_t>(a);
    }
    __asm volatile("dsb 0xf" ::: "memory");
    __asm volatile("isb 0xf" ::: "memory");
#else
    (void)p;
    (void)size;
#endif
}

} // namespace

extern "C" const ts_tracker_platform_t ts_tracker_platform_tracker = {
    .dir_open = dirOpen,
    .dir_next = dirNext,
    .dir_close = dirClose,
    .file_read = fileRead,
    .file_free = fileFree,
    .exec_alloc = execAlloc,
    .exec_free = execFree,
    .cache_sync = cacheSync,
    .require_thumb = 1, /* the M7 has no ARM state */
};

#endif /* FEATURE_SYNTH */
