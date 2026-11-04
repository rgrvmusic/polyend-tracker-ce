/*
 * tinysynth.h - portable integer synth module ABI.
 *
 * One core, loaded directly (.synthmodule) or compiled into a platform
 * wrapper (drumlogue, microKORG2, Schwung/Move, Blackfin).  Q15 audio,
 * integer phase, no libc, no allocation, no float in the core.
 *
 * MIT License.  Copyright (c) 2026 Gerard Braad.
 */
#ifndef TINYSYNTH_H
#define TINYSYNTH_H

#include <stddef.h>
#include <stdint.h>

#define TS_ABI_VERSION        0x00010000u
#define TS_MAX_PARAMS         9u
#define TS_MAX_VOICES         8u
#define TS_MAX_FRAMES         256u
#define TS_MAX_INSTANCE_BYTES 2048u

typedef int16_t ts_q15_t;   /* audio path                         */
typedef int32_t ts_q31_t;   /* ratio / coefficient intermediates  */

/* Param types.  The numbers match the logue SDK's k_unit_param_type_* so a
 * wrapper can copy the descriptor straight into a unit header. */
enum {
    TS_PTYPE_NONE = 0,
    TS_PTYPE_PERCENT,
    TS_PTYPE_DB,
    TS_PTYPE_CENTS,
    TS_PTYPE_SEMI,
    TS_PTYPE_OCT,
    TS_PTYPE_HERTZ,
    TS_PTYPE_KHERTZ,
    TS_PTYPE_BPM,
    TS_PTYPE_MSEC,
    TS_PTYPE_SEC,
    TS_PTYPE_ENUM,
    TS_PTYPE_STRINGS,
    TS_PTYPE_BITMAPS,
    TS_PTYPE_DRYWET,
    TS_PTYPE_PAN,
    TS_PTYPE_SPREAD,
    TS_PTYPE_ONOFF,
    TS_PTYPE_MIDI_NOTE
};

typedef struct ts_param {
    int16_t     min;
    int16_t     max;
    int16_t     center;
    int16_t     init;
    uint8_t     type;
    uint8_t     frac;
    const char *name;
} ts_param_t;

typedef struct ts_desc {
    uint32_t samplerate;
    uint16_t max_frames;    /* largest block the host will pass */
    uint8_t  input_channels;
    uint8_t  output_channels;
} ts_desc_t;

/*
 * The module vtable.  Fields are append-only: never reorder, never change a
 * signature.  A host reads the struct only after checking abi_version and
 * struct_size.
 *
 * render()      mono Q15, mixes all voices.
 * render_voice() one voice, `stride` samples between frames (1 = packed).
 *
 * A host must call one or the other, not both, for the same samples: each
 * advances voice state.
 */
typedef struct ts_module {
    uint32_t    abi_version;
    uint32_t    struct_size;
    const char *name;
    uint8_t     num_params;
    uint8_t     max_voices;
    uint16_t    max_frames;
    const ts_param_t *params;
    uint32_t    instance_size;

    int8_t  (*init)(void *self, const ts_desc_t *desc);
    void    (*reset)(void *self);
    void    (*render)(void *self, ts_q15_t *out, uint32_t frames);
    void    (*render_voice)(void *self, uint8_t voice, ts_q15_t *out,
                            uint32_t frames, uint32_t stride);
    void    (*set_param)(void *self, uint8_t id, int32_t value);
    int32_t (*get_param)(void *self, uint8_t id);

    void    (*note_on)(void *self, uint8_t note, uint8_t velocity);
    void    (*note_off)(void *self, uint8_t note);
    void    (*all_notes_off)(void *self);

    void    (*voice_on)(void *self, uint8_t voice, uint8_t note, uint8_t velocity);
    void    (*voice_off)(void *self, uint8_t voice);
    void    (*voice_pitch)(void *self, uint8_t voice, uint32_t note_q16);
    void    (*pitch_bend)(void *self, uint16_t bend);   /* 0..16383, 8192 centre */
    void    (*channel_pressure)(void *self, uint8_t pressure);
    void    (*aftertouch)(void *self, uint8_t note, uint8_t pressure);

    void    (*gate_on)(void *self, uint8_t velocity);
    void    (*gate_off)(void *self);
    void    (*set_tempo)(void *self, uint32_t tempo);

    /* 1 = output raw voice level; the host owns the amplitude envelope.
     * Used by oscillator-slot wrappers such as microKORG2. */
    void    (*set_env_bypass)(void *self, uint8_t bypass);

    /* The module's own word for a parameter value ("Saw"), or NULL for a
     * parameter it spells in numbers.  A compiled unit does not need this --
     * its readout is built from TS_PARAM_STR at compile time -- but a host
     * loads the module after its own header is fixed, so the same words can
     * only come from the module itself.
     *
     * `width` is how many characters the caller has room for, excluding the
     * terminator: a host must spell the knob's name in the same short column,
     * so it asks for a shorter word ("Sqr") than a compiled unit has room for
     * ("Square").  The module answers with the longest form that fits, or NULL
     * when none does.
     *
     * Appended, like every field after struct_size; a module built before it
     * existed has a shorter struct. */
    const char *(*param_str)(uint8_t id, int32_t value, uint8_t width);
} ts_module_t;

/* The vtable as it stood before param_str was appended.  A host must accept a
 * module that declares only this much: such a module offers no words, and
 * reading past its struct would be reading the module's own memory. */
#define TS_MODULE_BASE_BYTES ((uint32_t)offsetof(ts_module_t, param_str))
#define TS_MODULE_HAS_PARAM_STR(m) \
    ((m)->param_str != \
         (const char *(*)(uint8_t, int32_t, uint8_t))0 && \
     (m)->struct_size >= (uint32_t)(offsetof(ts_module_t, param_str) + \
                                    sizeof((m)->param_str)))

#if defined(TS_NO_EXPORT)
#define TS_EXPORT
#elif defined(__GNUC__)
#define TS_EXPORT __attribute__((visibility("default")))
#else
#define TS_EXPORT
#endif

/* The only dynamic symbol a .synthmodule needs. */
TS_EXPORT const ts_module_t *ts_module(void);

/* Host-side guards, usable as compile-time expressions. */
#define TS_ABI_MAJOR(v) ((v) >> 16)
#define TS_ABI_OK(v)    (TS_ABI_MAJOR(v) == TS_ABI_MAJOR(TS_ABI_VERSION))

#endif /* TINYSYNTH_H */
