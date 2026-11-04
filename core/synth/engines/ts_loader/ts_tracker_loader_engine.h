/*
 * wrap/tracker/ts_tracker_loader_engine.h - the Tracker's module loader engine.
 *
 * One Tracker synth engine that plays whatever .synthmodule the SD card holds
 * in /synthmodules.  It is the device half of wrap/tracker/ts_tracker_host.c:
 * the host scans and relocates the images once, and each Tracker voice gets
 * one module instance to drive, exactly like the compiled-in engine in
 * ts_tracker_engine.h.
 *
 * Parameter 0 is the module selector; the module's own parameters follow it,
 * so a module keeps all nine of its parameters.  Selecting a module changes
 * the parameter names and the voice count, which is what
 * invalidatesOtherMetadata is for: the framework rebuilds the editor, and the
 * Module handler rebuilds every voice's instance.
 *
 * The scan is lazy because createMetadata and createVoice run on the main
 * thread after the SD card is mounted, but renderVoiceAudio must never touch
 * the card.  It scans once; the host does not free images until the next scan,
 * so a voice may keep rendering from its module while the selector changes.
 *
 * MIT License.  Copyright (c) 2026 Gerard Braad.
 */
#ifndef TS_TRACKER_LOADER_ENGINE_H
#define TS_TRACKER_LOADER_ENGINE_H

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "PolySynth/config.h"
#include "PolySynth/sdk/SynthVoiceSDK.h"

#include "platform_tracker.h"
#include "ts_tracker_engine.h"
#include "ts_tracker_host.h"

namespace ts_tracker
{

enum class LoaderParamId : int
{
    kModule = 0,
    kP0,
    kP1,
    kP2,
    kP3,
    kP4,
    kP5,
    kP6,
    kP7,
    kP8,
};

constexpr int kModuleParamBase = 1;

/* Scan the module folder once.  Nothing is retried after a successful scan,
 * but an empty result is retried, because the editor can be built before the
 * card is mounted and the folder should appear once it is. */
inline int loaderScan()
{
    static int count = -2;

    if (count <= 0)
    {
        count = ts_tracker_host_scan(&ts_tracker_platform_tracker, TS_TRACKER_MODULE_DIR);
        if (count < 0)
        {
            count = 0;
        }
    }
    return count;
}

inline int clampModule(int index)
{
    const int count = loaderScan();

    return count > 0 ? std::clamp(index, 0, count - 1) : 0;
}

/* Shared with createMetadata so a voice built before the editor refreshed
 * still starts on the module the patch selected. */
inline int& selectedModule()
{
    static int index = 0;

    return index;
}

inline void selectModule(int index)
{
    selectedModule() = clampModule(index);
    if (loaderScan() > 0)
    {
        ts_tracker_host_select(selectedModule());
    }
}

class LoaderVoice : public polysynth::sdk::CustomSynthVoice<LoaderVoice, LoaderParamId>
{
  public:
    using Base = polysynth::sdk::CustomSynthVoice<LoaderVoice, LoaderParamId>;
    using Param = polysynth::Patch::Metadata::Param;
    using Macro = polysynth::Patch::Metadata::Macro;
    using Context = polysynth::InstrumentVoice::InstrumentVoiceContext;

    LoaderVoice() { rebuild(); }
    ~LoaderVoice() override { release(); }

    bool isActive() final { return ok_ && started_ && nonzero_; }

    void onNoteOn(const Context& /*ctx*/,
                  polysynth::note_t note,
                  float velocity,
                  bool /*isLegatoStep*/) final
    {
        if (!ok_)
        {
            return;
        }
        note_ = note;
        velocity_ = std::clamp(velocity, 0.0f, 1.0f);
        module_->voice_on(mem_,
                          0,
                          static_cast<std::uint8_t>(note),
                          static_cast<std::uint8_t>(std::lround(velocity_ * 127.0f)));
        started_ = true;
        /* One block always renders, even if the module starts at silence. */
        nonzero_ = true;
    }

    void onNoteOff(const Context& /*ctx*/, polysynth::note_t /*note*/) final
    {
        if (ok_)
        {
            module_->voice_off(mem_, 0);
        }
    }

    void renderVoiceAudio(const Context& /*ctx*/,
                          std::optional<polysynth::AudioBufferStereoRef> /*input*/,
                          polysynth::AudioBufferStereoRef outputBuffer,
                          std::span<polysynth::AudioBufferRef> /*auxOut*/) final
    {
        auto left = outputBuffer[0];
        auto right = outputBuffer[1];
        const std::size_t frames = left.size();

        std::fill(right.begin(), right.end(), 0.0f);

        if (!ok_)
        {
            std::fill(left.begin(), left.end(), 0.0f);
            return;
        }

        module_->voice_pitch(
          mem_, 0, note_q16(static_cast<float>(note_) + this->pitchBendSemitones));

        ts_q15_t block[polysynth::kAudioBufferSize];
        module_->render_voice(mem_, 0, block, static_cast<std::uint32_t>(frames), 1);

        bool nonzero = false;
        for (std::size_t i = 0; i < frames; ++i)
        {
            left[i] = static_cast<float>(block[i]) * (1.0f / 32768.0f);
            nonzero = nonzero || block[i] != 0;
        }
        nonzero_ = nonzero;
    }

    /* Param 0: the selector.  Always (re)build, since createMetadata may have
     * moved the selection already and the instance must follow it anyway. */
    static void applyModule(LoaderVoice& voice, int index)
    {
        selectModule(index);
        voice.rebuild();
    }

    static void applyModuleParam(LoaderVoice& voice, int index, float value)
    {
        voice.applyParam(index, value);
    }

    static void createMetadata(polysynth::Patch& patch,
                               polysynth::Patch::Metadata& metadata)
    {
        const int found = loaderScan();

        int selected = 0;
        /* The patch's old metadata is what says whether param 0 exists: on the
         * first creation for this instrument it does not, and asking the
         * patch for a value it has no slot for is not safe. */
        if (patch.getMetadata().params.size() > 0)
        {
            if (auto value = patch.getParamValue(static_cast<int>(LoaderParamId::kModule));
                value.has_value())
            {
                selected = static_cast<int>(std::lround(*value));
            }
        }
        selectModule(selected);

        const ts_module_t* m = found > 0 ? ts_tracker_host_module() : nullptr;
        const int moduleParams = m ? std::min<int>(m->num_params, TS_MAX_PARAMS) : 0;

        metadata.params.assign(static_cast<std::size_t>(kModuleParamBase + moduleParams), Param{});
        metadata.outputChannels = polysynth::Patch::Metadata::OutputChannels::kMono;
        metadata.maxVoices =
          std::max(1, std::min<int>(m ? m->max_voices : 1, static_cast<int>(TS_MAX_VOICES)));

        /* The selector spells out the module's name, not its index. */
        {
            Param& sel = metadata.params[0];
            sel.displayName = "Module";
            sel.defaultValue = static_cast<float>(selectedModule());
            sel.minValue = 0.0f;
            sel.maxValue = static_cast<float>(found > 0 ? found - 1 : 0);
            sel.isEnum = true;
            sel.invalidatesOtherMetadata = true;
            sel.valueFormatter = [found](const polysynth::Patch&,
                                         polysynth::Patch::paramvalue_t value)
            {
                const int index = std::clamp(static_cast<int>(std::lround(value)),
                                             0,
                                             found > 0 ? found - 1 : 0);
                const ts_tracker_entry_t* entry = ts_tracker_host_entry(index);
                return std::string(entry && entry->name ? entry->name : "---");
            };
        }

        for (int i = 0; i < moduleParams; ++i)
        {
            const ts_param_t& p = m->params[i];
            Param& def = metadata.params[static_cast<std::size_t>(kModuleParamBase + i)];

            def.displayName = p.name ? p.name : "";
            def.defaultValue = static_cast<float>(p.init);
            def.minValue = static_cast<float>(p.min);
            def.maxValue = static_cast<float>(p.max);
            def.editCurve = Param::Curve::kLinear;
            def.macroOperation = Param::MacroOperation::kAdd;
            def.isEnum = p.type == TS_PTYPE_ENUM || p.type == TS_PTYPE_ONOFF;

            if (m->param_str && TS_MODULE_HAS_PARAM_STR(m))
            {
                const std::uint8_t id = static_cast<std::uint8_t>(i);
                def.valueFormatter = [m, id](const polysynth::Patch&,
                                             polysynth::Patch::paramvalue_t v)
                {
                    const char* s = m->param_str(id, static_cast<std::int32_t>(v), 18);
                    return std::string(s ? s : "");
                };
            }
        }

        /* Dispatch table: one entry per parameter id, in patch id order.  These
         * are captureless lambdas, which is what the SDK stores.  The base
         * class's table starts empty, so size it first, as the SDK's own
         * engines do -- writing into a static_vector past its size is not a
         * write to a slot. */
        if (Base::paramHandlers.empty())
        {
            Base::paramHandlers.resize(static_cast<std::size_t>(LoaderParamId::kP8) + 1u);
        }
        Base::paramHandlers[0] = [](LoaderVoice& v, polysynth::Patch::optional_paramvalue_t val)
        { applyModule(v, static_cast<int>(std::lround(*val))); };
        Base::paramHandlers[1] = [](LoaderVoice& v, polysynth::Patch::optional_paramvalue_t val)
        { applyModuleParam(v, 0, *val); };
        Base::paramHandlers[2] = [](LoaderVoice& v, polysynth::Patch::optional_paramvalue_t val)
        { applyModuleParam(v, 1, *val); };
        Base::paramHandlers[3] = [](LoaderVoice& v, polysynth::Patch::optional_paramvalue_t val)
        { applyModuleParam(v, 2, *val); };
        Base::paramHandlers[4] = [](LoaderVoice& v, polysynth::Patch::optional_paramvalue_t val)
        { applyModuleParam(v, 3, *val); };
        Base::paramHandlers[5] = [](LoaderVoice& v, polysynth::Patch::optional_paramvalue_t val)
        { applyModuleParam(v, 4, *val); };
        Base::paramHandlers[6] = [](LoaderVoice& v, polysynth::Patch::optional_paramvalue_t val)
        { applyModuleParam(v, 5, *val); };
        Base::paramHandlers[7] = [](LoaderVoice& v, polysynth::Patch::optional_paramvalue_t val)
        { applyModuleParam(v, 6, *val); };
        Base::paramHandlers[8] = [](LoaderVoice& v, polysynth::Patch::optional_paramvalue_t val)
        { applyModuleParam(v, 7, *val); };
        Base::paramHandlers[9] = [](LoaderVoice& v, polysynth::Patch::optional_paramvalue_t val)
        { applyModuleParam(v, 8, *val); };

        /* Six editor tiles, two parameters each.  The first is the module
         * choice; the rest follow the module's own parameters. */
        using EditorSection = polysynth::Patch::Metadata::EditorLayout::EditorSection;
        using EditorElement = polysynth::Patch::Metadata::EditorLayout::EditorElement;

        static constexpr const char* kSectionOrder[] = { "Voice",
                                                         "Oscillator",
                                                         "Filter",
                                                         "Amplifier",
                                                         "Modulation",
                                                         "FX" };

        metadata.layoutFlat.sections.clear();
        for (int base = 0; base <= moduleParams; base += 2)
        {
            if (metadata.layoutFlat.sections.size() >=
                sizeof kSectionOrder / sizeof kSectionOrder[0])
            {
                break;
            }
            EditorSection section;
            section.name = kSectionOrder[metadata.layoutFlat.sections.size()];
            section.shortName = section.name;
            for (int id = base; id < base + 2 && id <= moduleParams; ++id)
            {
                section.elements.push_back(
                  EditorElement{ .type = EditorElement::Type::kParam, .id = id });
            }
            metadata.layoutFlat.sections.push_back(std::move(section));
        }
        metadata.layout23 = metadata.layoutFlat;

        /* The six user macros follow the module's first six parameters. */
        metadata.initDefaultMacros = [m, moduleParams](std::vector<Macro>& macros)
        {
            const int n = std::min<int>(moduleParams, static_cast<int>(polysynth::kUserMacroCount));
            for (int i = 0; i < n; ++i)
            {
                macros[static_cast<std::size_t>(i)].displayName =
                  m->params[i].name ? m->params[i].name : "";
                macros[static_cast<std::size_t>(i)].modifiers = {
                    Macro::ParamModifier{ .paramId = kModuleParamBase + i, .amount = 1.0f }
                };
            }
        };
    }

  private:
    void release()
    {
        if (mem_ != nullptr)
        {
            ts_tracker_host_instance_free(mem_);
            mem_ = nullptr;
        }
        module_ = nullptr;
        ok_ = false;
    }

    /* A fresh module instance for the selected module, with the parameter
     * values this voice has already been given applied on top of the module
     * defaults.  Runs when the voice is created and when Module changes, both
     * off the audio thread. */
    void rebuild()
    {
        release();
        if (loaderScan() <= 0)
        {
            return;
        }

        selectModule(selectedModule());
        module_ = ts_tracker_host_module();
        mem_ = ts_tracker_host_instance_new();
        if (module_ == nullptr || mem_ == nullptr)
        {
            module_ = nullptr;
            return;
        }
        ok_ = true;

        for (int i = 0; i < static_cast<int>(TS_MAX_PARAMS); ++i)
        {
            if (values_[i].has_value())
            {
                applyParam(i, *values_[i]);
            }
        }
    }

    void applyParam(int index, float value)
    {
        if (index < 0 || index >= static_cast<int>(TS_MAX_PARAMS))
        {
            return;
        }
        values_[index] = value;

        if (!ok_ || index >= module_->num_params)
        {
            return;
        }
        std::int32_t v = static_cast<std::int32_t>(std::lround(value));
        v = std::max<std::int32_t>(v, module_->params[index].min);
        v = std::min<std::int32_t>(v, module_->params[index].max);
        module_->set_param(mem_, static_cast<std::uint8_t>(index), v);
    }

    void* mem_ = nullptr;
    const ts_module_t* module_ = nullptr;
    bool ok_ = false;
    bool started_ = false;
    bool nonzero_ = false;
    polysynth::note_t note_ = 60;
    float velocity_ = 1.0f;
    std::optional<float> values_[TS_MAX_PARAMS];
};

/* Build the InstrumentType registered as kTsLoader. */
inline std::unique_ptr<polysynth::InstrumentType> createLoaderEngine()
{
    auto type = std::make_unique<polysynth::InstrumentType>();
    type->name = "TS";
    type->createMetadata = [](polysynth::Patch& patch, polysynth::Patch::Metadata& metadata)
    {
        LoaderVoice::createMetadata(patch, metadata);
        metadata.instrumentTypeId =
          static_cast<int>(polysynth::InstrumentTypeId::kTsLoader);
    };
    type->createVoice = [](int /*voiceIdx*/) -> std::unique_ptr<polysynth::InstrumentVoice>
    { return std::make_unique<LoaderVoice>(); };
    return type;
}

} // namespace ts_tracker

#endif /* TS_TRACKER_LOADER_ENGINE_H */
