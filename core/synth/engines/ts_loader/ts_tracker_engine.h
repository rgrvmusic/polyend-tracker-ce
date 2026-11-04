/*
 * wrap/tracker/ts_tracker_engine.h - Polyend Tracker+ / Tracker Mini engine.
 *
 * The Tracker has no module loader: a synth engine is C++ compiled into the
 * .ptf firmware and named in core/synth/registry.cpp.  This header is the
 * bridge from one such engine to one tinysynth module, so a module source
 * only needs a generated one-function glue file (tools/tracker-engine.sh).
 *
 * One Tracker voice owns one module instance and drives module voice 0 of it:
 * the Tracker's voice allocator already decides polyphony, and an instance per
 * voice keeps the module's state where the SDK expects per-voice state.  The
 * module renders one Q15 mono block, which is converted to the SDK's float
 * sample here; PolySynth pans the mono result itself.
 *
 * The Tracker's audio block is 128 frames at 44100 Hz, both fixed by the SDK.
 *
 * MIT License.  Copyright (c) 2026 Gerard Braad.
 */
#ifndef TS_TRACKER_ENGINE_H
#define TS_TRACKER_ENGINE_H

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "PolySynth/sdk/SynthVoiceSDK.h"

extern "C" {
#include "tinysynth.h"
}

namespace ts_tracker
{

/* The Tracker's parameter ids are a contiguous 0..N enum.  TS_MAX_PARAMS is
 * the module ABI's cap, so the enum is fixed here and the module's own count
 * decides how many entries are filled. */
enum class ParamId : int
{
    kP0 = 0,
    kP1,
    kP2,
    kP3,
    kP4,
    kP5,
    kP6,
    kP7,
    kP8,
};

/* Section names the Patch Editor maps to hardware icons; see
 * docs/custom_synth_sdk.md.  Params are paired into sections in this order. */
constexpr const char *kSectionNames[] = { "Oscillator", "Filter",  "Amplifier",
                                          "Modulation", "Voice",   "FX" };

inline std::uint32_t note_q16(float pitch)
{
    if (pitch <= 0.0f)
    {
        return 0u;
    }
    if (pitch >= 127.0f)
    {
        return 127u << 16;
    }
    return static_cast<std::uint32_t>(pitch * 65536.0f + 0.5f);
}

template<const ts_module_t *(*GetModule)()> class TrackerVoice
  : public polysynth::sdk::CustomSynthVoice<TrackerVoice<GetModule>, ParamId>
{
  public:
    using Base = polysynth::sdk::CustomSynthVoice<TrackerVoice<GetModule>, ParamId>;
    using Param = polysynth::Patch::Metadata::Param;
    using Macro = polysynth::Patch::Metadata::Macro;
    using Context = polysynth::InstrumentVoice::InstrumentVoiceContext;

    explicit TrackerVoice(int /*voiceIdx*/)
    {
        const ts_module_t *m = GetModule();
        const std::size_t bytes =
          m && m->instance_size ? static_cast<std::size_t>(m->instance_size) : 1u;

        mem_.reset(new std::uint8_t[bytes]);
        std::memset(mem_.get(), 0, bytes);

        ts_desc_t desc;
        desc.samplerate = static_cast<std::uint32_t>(polysynth::sdk::kSampleRate);
        desc.max_frames = static_cast<std::uint16_t>(polysynth::kAudioBufferSize);
        desc.input_channels = 0;
        desc.output_channels = 1;

        ok_ = m && m->init && m->init(mem_.get(), &desc) == 0;
        if (ok_)
        {
            for (std::uint8_t i = 0; i < m->num_params && i < TS_MAX_PARAMS; ++i)
            {
                m->set_param(mem_.get(), i, m->params[i].init);
            }
        }
    }

    bool isActive() final { return ok_ && started_ && nonzero_; }

    void onNoteOn(const Context & /*ctx*/,
                  polysynth::note_t note,
                  float velocity,
                  bool /*isLegatoStep*/) final
    {
        const ts_module_t *m = GetModule();
        if (!ok_)
        {
            return;
        }
        note_ = note;
        velocity_ = std::clamp(velocity, 0.0f, 1.0f);
        m->voice_on(mem_.get(), 0, static_cast<std::uint8_t>(note),
                    static_cast<std::uint8_t>(std::lround(velocity_ * 127.0f)));
        started_ = true;
        /* One block always renders, even if the module starts at silence. */
        nonzero_ = true;
    }

    void onNoteOff(const Context & /*ctx*/, polysynth::note_t /*note*/) final
    {
        if (ok_)
        {
            GetModule()->voice_off(mem_.get(), 0);
        }
    }

    void renderVoiceAudio(const Context & /*ctx*/,
                          std::optional<polysynth::AudioBufferStereoRef> /*input*/,
                          polysynth::AudioBufferStereoRef outputBuffer,
                          std::span<polysynth::AudioBufferRef> /*auxOut*/) final
    {
        auto left = outputBuffer[0];
        auto right = outputBuffer[1];
        const std::size_t frames = left.size();

        std::fill(right.begin(), right.end(), 0.0f);

        const ts_module_t *m = GetModule();
        if (!ok_)
        {
            std::fill(left.begin(), left.end(), 0.0f);
            return;
        }

        /* The base class folds pitch bend into semitones; hand the module the
         * absolute note so it can re-derive its phase increment each block. */
        m->voice_pitch(mem_.get(), 0,
                       note_q16(static_cast<float>(note_) + this->pitchBendSemitones));

        ts_q15_t block[polysynth::kAudioBufferSize];
        m->render_voice(mem_.get(), 0, block, static_cast<std::uint32_t>(frames), 1);

        bool nonzero = false;
        for (std::size_t i = 0; i < frames; ++i)
        {
            left[i] = static_cast<float>(block[i]) * (1.0f / 32768.0f);
            nonzero = nonzero || block[i] != 0;
        }
        nonzero_ = nonzero;
    }

    /* Called by the CustomSynthVoice dispatch table. */
    static constexpr void setParam(TrackerVoice &voice,
                                   polysynth::Patch::optional_paramvalue_t value,
                                   int id)
    {
        voice.applyParam(id, *value);
    }

    static void createMetadata(polysynth::Patch & /*patch*/,
                               polysynth::Patch::Metadata &metadata)
    {
        const ts_module_t *m = GetModule();
        const int count = m ? std::min<int>(m->num_params, TS_MAX_PARAMS) : 0;

        metadata.params.assign(static_cast<std::size_t>(count), Param{});
        metadata.outputChannels = polysynth::Patch::Metadata::OutputChannels::kMono;
        metadata.maxVoices = std::max(1, std::min<int>(m ? m->max_voices : 1,
                                                       static_cast<int>(TS_MAX_VOICES)));

        for (int i = 0; i < count; ++i)
        {
            const ts_param_t &p = m->params[i];
            Param def;

            def.displayName = p.name ? p.name : "";
            def.defaultValue = static_cast<float>(p.init);
            def.minValue = static_cast<float>(p.min);
            def.maxValue = static_cast<float>(p.max);
            def.editCurve = Param::Curve::kLinear;
            def.macroOperation = Param::MacroOperation::kAdd;
            def.isEnum = p.type == TS_PTYPE_ENUM || p.type == TS_PTYPE_ONOFF;

            /* A module compiled with the word table can name its own values
             * ("Saw"); one built before it existed shows the number. */
            if (m->param_str && TS_MODULE_HAS_PARAM_STR(m))
            {
                const std::uint8_t id = static_cast<std::uint8_t>(i);
                def.valueFormatter = [m, id](const polysynth::Patch &,
                                             polysynth::Patch::paramvalue_t v)
                {
                    const char *s = m->param_str(id, static_cast<std::int32_t>(v), 18);
                    return std::string(s ? s : "");
                };
            }

            metadata.params[static_cast<std::size_t>(i)] = std::move(def);
        }

        /* The registry checks param handlers by index, so wire one per id.
         * These are captureless lambdas: the SDK stores them as plain
         * function pointers. */
        Base::paramHandlers[0] = [](TrackerVoice &v, polysynth::Patch::optional_paramvalue_t val)
        { setParam(v, val, 0); };
        Base::paramHandlers[1] = [](TrackerVoice &v, polysynth::Patch::optional_paramvalue_t val)
        { setParam(v, val, 1); };
        Base::paramHandlers[2] = [](TrackerVoice &v, polysynth::Patch::optional_paramvalue_t val)
        { setParam(v, val, 2); };
        Base::paramHandlers[3] = [](TrackerVoice &v, polysynth::Patch::optional_paramvalue_t val)
        { setParam(v, val, 3); };
        Base::paramHandlers[4] = [](TrackerVoice &v, polysynth::Patch::optional_paramvalue_t val)
        { setParam(v, val, 4); };
        Base::paramHandlers[5] = [](TrackerVoice &v, polysynth::Patch::optional_paramvalue_t val)
        { setParam(v, val, 5); };
        Base::paramHandlers[6] = [](TrackerVoice &v, polysynth::Patch::optional_paramvalue_t val)
        { setParam(v, val, 6); };
        Base::paramHandlers[7] = [](TrackerVoice &v, polysynth::Patch::optional_paramvalue_t val)
        { setParam(v, val, 7); };
        Base::paramHandlers[8] = [](TrackerVoice &v, polysynth::Patch::optional_paramvalue_t val)
        { setParam(v, val, 8); };

        /* Up to six Patch Editor tiles, two parameters each. */
        using EditorSection = polysynth::Patch::Metadata::EditorLayout::EditorSection;
        using EditorElement = polysynth::Patch::Metadata::EditorLayout::EditorElement;

        metadata.layoutFlat.sections.clear();
        for (int i = 0; i < count; i += 2)
        {
            EditorSection section;
            section.name = kSectionNames[(i / 2) % 6];
            section.shortName = section.name;
            for (int j = i; j < std::min(i + 2, count); ++j)
            {
                section.elements.push_back(
                  EditorElement{ .type = EditorElement::Type::kParam, .id = j });
            }
            metadata.layoutFlat.sections.push_back(std::move(section));
        }
        metadata.layout23 = metadata.layoutFlat;

        /* The six user macros follow the first six parameters, so every module
         * gets usable performance controls without its own macro table. */
        metadata.initDefaultMacros = [m, count](std::vector<Macro> &macros)
        {
            const int n =
              std::min<int>(count, static_cast<int>(polysynth::kUserMacroCount));
            for (int i = 0; i < n; ++i)
            {
                macros[static_cast<std::size_t>(i)].displayName =
                  m->params[i].name ? m->params[i].name : "";
                macros[static_cast<std::size_t>(i)].modifiers = {
                    Macro::ParamModifier{ .paramId = i, .amount = 1.0f }
                };
            }
        };
    }

  private:
    void applyParam(int id, float value)
    {
        const ts_module_t *m = GetModule();
        if (!ok_ || id >= static_cast<int>(m->num_params))
        {
            return;
        }
        std::int32_t v = static_cast<std::int32_t>(std::lround(value));
        v = std::max<std::int32_t>(v, m->params[id].min);
        v = std::min<std::int32_t>(v, m->params[id].max);
        m->set_param(mem_.get(), static_cast<std::uint8_t>(id), v);
    }

    std::unique_ptr<std::uint8_t[]> mem_;
    bool ok_ = false;
    bool started_ = false;
    bool nonzero_ = false;
    polysynth::note_t note_ = 60;
    float velocity_ = 1.0f;
};

/* Build the InstrumentType the Tracker registry stores.  `GetModule` must be
 * the module's renamed entry point (tools/tracker-engine.sh compiles the
 * module with -Dts_module=ts_<name>_module), and `instrumentTypeId` must be
 * the enum the firmware's config.h declares for this engine. */
template<const ts_module_t *(*GetModule)()>
std::unique_ptr<polysynth::InstrumentType>
makeEngine(const char *name, int instrumentTypeId)
{
    using Voice = TrackerVoice<GetModule>;

    auto type = std::make_unique<polysynth::InstrumentType>();
    type->name = name ? name : "";
    type->createMetadata = [instrumentTypeId](polysynth::Patch &patch,
                                              polysynth::Patch::Metadata &metadata)
    { Voice::createMetadata(patch, metadata); metadata.instrumentTypeId = instrumentTypeId; };
    type->createVoice = [](int voiceIdx) -> std::unique_ptr<polysynth::InstrumentVoice>
    { return std::make_unique<Voice>(voiceIdx); };
    return type;
}

} // namespace ts_tracker

#endif /* TS_TRACKER_ENGINE_H */
