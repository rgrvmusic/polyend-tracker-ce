/*
 * wrap/tracker/platform_tracker.h - the Tracker+ / Tracker Mini platform.
 *
 * MIT License.  Copyright (c) 2026 Gerard Braad.
 */
#ifndef TS_TRACKER_PLATFORM_TRACKER_H
#define TS_TRACKER_PLATFORM_TRACKER_H

#include "ts_tracker_host.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Where the firmware looks for loadable modules.  A top-level folder next to
 * "samples" and "patches": the Tracker's own browser filters do not know the
 * extension, so it never shows these files anywhere else. */
#define TS_TRACKER_MODULE_DIR "/synthmodules"

extern const ts_tracker_platform_t ts_tracker_platform_tracker;

#ifdef __cplusplus
}
#endif

#endif /* TS_TRACKER_PLATFORM_TRACKER_H */
