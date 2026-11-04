#include "instrumentEditor/instrumentEditor.h"
#if GUI_HANDLE_SYNTH

#include "core/interfacePopups.h"
#include "mtPadBoard.h"
#include "mtPadsBacklight.h"

#include "PolySynth/Patch.h"
#include "cr_section_macros.h"
#include "fileManager.h"
#include "mtAudioEngine.h"
#include "util_math.h"
#include "util_string.h"

extern uint8_t externalRamScratchBuff[];
static_assert(kMaxFilesListLength * (kMaxFileNameLen + 1) + kMaxFilesListLength * sizeof(char**) <=
                SHARED_SCRATCH_MEMORY_SIZE,
              "Synth patch browser buffers exceed shared scratch size");

static const uint16_t framesPlacesSynth[1][4] = {
    { (800 / 8) * 6 + 1, 31, 800 / 4 - 3, 391 },
};

const char* const synthModelNames[Synth::kSynthTypes.size()] = {
    "ACD", "FAT", "VAP", "WTFM", "PERC", "DIRT", "CHIP", "TS",
};

static const int synthModelListImages[Synth::kSynthTypes.size()] = {
    enDisplayBitmaps::kBitmapMiniACD,  enDisplayBitmaps::kBitmapMiniFAT,
    enDisplayBitmaps::kBitmapMiniVAP,  enDisplayBitmaps::kBitmapMiniWTFM,
    enDisplayBitmaps::kBitmapMiniPERC, enDisplayBitmaps::kBitmapMiniDIRT,
    enDisplayBitmaps::kBitmapMiniCHIP, enDisplayBitmaps::kBitmapMiniCHIP
};

static cInstrumentEditor* IE = &instrumentEditor;

static void functSynthActionButtons(uint8_t button);
static void functSynthLoad();
static void functSynthEditPatch();

static void functSynthListButton(uint8_t button, uint8_t state);
static void functSynthConfirmLoad();
static void functSynthCancelLoad();
static void functSynthListEncoder(int16_t value);
static void functSynthLoadMiniMiddle(uint8_t state);

static void functSynthEncoder(int16_t value);
static void functSynthUp();
static void functSynthDown();
static void functSynthDelete();
static void functSynthMiniMiddle(uint8_t state);
static void functSynthPads(uint8_t pad, uint8_t state, int16_t velo);
static void functSynthLeft();
static void functSynthRight();
static void functSynthShift(uint8_t state);

void cInstrumentEditor::exitSynthEditor()
{
    if (!isSynthInstrument(mtProject.values.lastUsedInstrument))
    {
        return;
    }

    int syntPartIdx = mtProject.values.lastUsedInstrument - SYNTH_INSTRUMENTS_START_IDX;
    assert(syntPartIdx >= 0 && syntPartIdx < kSynthInstrumentsCount);

    if (buttons->isButtonPressed(interfaceButton4))
    {
        IE->getSynth()->triggerTestNote(syntPartIdx, IE->kSynthPreviewNote1, false);
    }

    if (buttons->isButtonPressed(interfaceButton5))
    {
        IE->getSynth()->triggerTestNote(syntPartIdx, IE->kSynthPreviewNote2, false);
    }
}

void cInstrumentEditor::setSynthParamsScreenFunct()
{
    setBaseScreenFunct();

    ip->clearButtonsRange(interfaceButton0, interfaceButton7);
    ip->clearAllPots();

    for (uint8_t i = interfaceButton0; i < interfaceButton6; i++)
    {
        ip->setButtonObj(i, buttonPress, functSynthActionButtons);
    }

    ip->setButtonObj(interfaceButton6, buttonPress, functSynthLoad);
    ip->setButtonObj(interfaceButton7, buttonPress, functSynthEditPatch);

    ip->setPotObj(interfacePot0, functSynthEncoder);

    ip->setButtonObj(interfaceButtonShift, functSynthShift);

    ip->setButtonObj(interfaceButtonLeft, buttonPress, functSynthLeft);
    ip->setButtonObj(interfaceButtonRight, buttonPress, functSynthRight);
    ip->setButtonObj(interfaceButtonUp, buttonPress, functSynthUp);
    ip->setButtonObj(interfaceButtonDown, buttonPress, functSynthDown);

    ip->setButtonObj(interfaceButtonDelete, buttonPress, functSynthDelete);
    ip->setButtonObj(interfaceButtonMiniMiddle, functSynthMiniMiddle);
    ip->setPadsGlobal(functSynthPads);
    ip->clearButton(interfaceButtonInsert);
}

void cInstrumentEditor::changeSynthParamValue(int value, ValueChangeSource source)
{
    bool invert = source != kEncoder || interfaceGlobals.invertValForList;

    switch (selectedPlace[mtInstEditModeSynth])
    {
        case 0: changeSynthModel(invert ? -value : value); break;
        case 1: changeSynthPolyphony(value); break;
        case 2: changeSynthGain(value); break;
        case 3: changeSynthPanning(value); break;
        case 4: changeSynthReverbSend(value); break;
        case 5: changeSynthDelaySend(value); break;
    }
}

static void functSynthMiniMiddle(uint8_t state)
{
    IE->synthCommonPreviewButtonFunct(state);
}

static void functSynthPads(uint8_t pad, uint8_t state, int16_t)
{
    IE->playSynthNoteByPad(pad, state);
}

void cInstrumentEditor::synthCommonPreviewButtonFunct(uint8_t state)
{
    static uint8_t lastState = buttonRelease;

    if (state == lastState)
    {
        return;
    }

    lastState = state;

    IE->playSynthNoteByPad(12, state);
}

void cInstrumentEditor::playSynthNoteByPad(uint8_t pad, uint8_t state, uint8_t customLightPad)
{
    if (state == buttonPress)
    {
        padsBacklight.setFrontLayer(
          1, mtConfig.values.padsLightFront, customLightPad < 48 ? customLightPad : pad);
        uint8_t noteFromPad = mtPadBoard.getNoteFromPad(pad);
        sequencer.handleNoteOn(Sequencer::GRID_OUTSIDE_PATTERN, noteFromPad, -1, pad);
    }
    else if (state == buttonRelease)
    {
        padsBacklight.setFrontLayer(0, 0, customLightPad < 48 ? customLightPad : pad);
        uint8_t noteFromPad = mtPadBoard.getNoteFromPad(pad);
        sequencer.handleNoteOff(Sequencer::GRID_OUTSIDE_PATTERN, noteFromPad, 0, pad);
    }
}

static void functSynthEncoder(int16_t value)
{
    IE->changeSynthParamValue(value, IE->kEncoder);
}

static void functSynthUp()
{
    IE->changeSynthParamValue(1);
}

static void functSynthDown()
{
    IE->changeSynthParamValue(-1);
}

static void functSynthDelete()
{
    switch (IE->selectedPlace[mtInstEditModeSynth])
    {
        // case 0: IE->setDefaultSynthModel(); break;
        // case 1: IE->setDefaultSynthPolyphony(); break;
        case 2: IE->setDefaultSynthGain(); break;
        case 3: IE->setDefaultSynthPanning(); break;
        case 4: IE->setDefaultSynthReverb(); break;
        case 5: IE->setDefaultSynthDelay(); break;
        default: break;
    }
}

static void functSynthLeft()
{
    if (IE->selectedPlace[mtInstEditModeSynth] > 0)
    {
        IE->selectedPlace[mtInstEditModeSynth]--;
    }

    IE->activateLabelsBorder();
}

static void functSynthRight()
{
    if (IE->selectedPlace[mtInstEditModeSynth] < IE->frameData.placesCount - 1)
    {
        IE->selectedPlace[mtInstEditModeSynth]++;
    }

    IE->activateLabelsBorder();
}

static void functSynthShift(uint8_t state)
{
    if (state == buttonPress)
    {
        if (mtPopups.isNoteKeyboardPopupActive())
        {
            mtPopups.toggleNoteKeyboardPopup(InterfacePopups::noteKeyboardOption::kShowEditButtons);
        }
    }
    else if (state == buttonRelease)
    {
        if (mtPopups.isNoteKeyboardPopupActive())
        {
            mtPopups.toggleNoteKeyboardPopup(InterfacePopups::noteKeyboardOption::kForceShow);
        }
    }
}

static void functSynthActionButtons(uint8_t button)
{
    if (IE->mode != mtInstEditModeSynth)
    {
        return;
    }

    if (button == 0 && IE->selectedPlace[IE->mode] == 0)
    {
        IE->rollListOver = true;
        IE->changeSynthModel(1);
        IE->rollListOver = false;
    }

    IE->selectedPlace[IE->mode] = button;

    IE->activateLabelsBorder();
}

static void functSynthLoad()
{
    IE->browsePatchList();
    IE->showSynthPatchBrowser();

    IE->ip->clearAllPots();
    IE->ip->clearAllButtons();

    IE->setPlayButtonAction();
    IE->ip->setButtonObj(interfaceButton4, functSynthListButton);
    IE->ip->setButtonObj(interfaceButton5, functSynthListButton);
    IE->ip->setButtonObj(interfaceButton6, buttonPress, functSynthCancelLoad);
    IE->ip->setButtonObj(interfaceButton7, buttonPress, functSynthConfirmLoad);

    IE->ip->setButtonObj(interfaceButtonUp, functSynthListButton);
    IE->ip->setButtonObj(interfaceButtonDown, functSynthListButton);

    IE->ip->setPotObj(interfacePot0, functSynthListEncoder);
    IE->ip->setButtonObj(interfaceButtonMiniMiddle, functSynthLoadMiniMiddle);
}

static void functSynthConfirmLoad()
{
    if (IE->list2Data.length == 0)
    {
        return;
    }

    int syntPartIdx = instrumentIdxToMidiChannel(mtProject.values.lastUsedInstrument) - 1;
    assert(syntPartIdx >= 0 && syntPartIdx < kSynthInstrumentsCount);

    IE->getSynth()->triggerTestNote(syntPartIdx, IE->kSynthPreviewNote1, false);
    IE->getSynth()->triggerTestNote(syntPartIdx, IE->kSynthPreviewNote2, false);

    assert(!isOutOfRange(syntPartIdx, 0, kSynthParts - 1));

    if (IE->previousPatchName == IE->getSynth()->getPatchName(syntPartIdx))
    {
        // force reload current patch from Patches folder
        // to be sure to update the patch editor parameters
        if (!IE->getSynth()->loadPatch(syntPartIdx, IE->patchListDataName[IE->patchListPosition]))
        {
            mtPopups.show(0, "Error loading patch");
        }
    }

    polystrncpy(mtProject.values.synthPart[syntPartIdx].patchName,
                IE->getSynth()->getPatchName(syntPartIdx).c_str(),
                sizeof(SynthPartSettings::patchName));

    IE->getSynth()->initSynthPartMacrosValues(syntPartIdx, mtProject.values.synthPart[syntPartIdx]);
    engine.refreshSynthInstrumentParams(mtProject.values.synthPart[syntPartIdx], syntPartIdx);

    IE->getSynth()->savePatchToWorkspace(syntPartIdx);

    newFileManager.setProjectStructChanged();

    IE->showSynthParamsScreen();
    IE->setSynthParamsScreenFunct();
}

static void functSynthCancelLoad()
{
    IE->revertPatch();

    int syntPartIdx = instrumentIdxToMidiChannel(mtProject.values.lastUsedInstrument) - 1;
    assert(syntPartIdx >= 0 && syntPartIdx < kSynthInstrumentsCount);

    IE->getSynth()->triggerTestNote(syntPartIdx, IE->kSynthPreviewNote1, false);
    IE->getSynth()->triggerTestNote(syntPartIdx, IE->kSynthPreviewNote2, false);

    engine.refreshSynthInstrumentParams(mtProject.values.synthPart[syntPartIdx], syntPartIdx);

    IE->showSynthParamsScreen();
    IE->setSynthParamsScreenFunct();
}

static void functSynthListButton(uint8_t button, uint8_t state)
{
    switch (button)
    {
        case interfaceButton4:
        case interfaceButton5:
        {
            if (IE->list2Data.length == 0)
            {
                return;
            }

            int syntPartIdx = mtProject.values.lastUsedInstrument - SYNTH_INSTRUMENTS_START_IDX;
            assert(syntPartIdx >= 0 && syntPartIdx < kSynthInstrumentsCount);
            int note = button == interfaceButton5 ? 60 : 60 - 2 * 12;
            if (state == buttonPress)
            {
                IE->getSynth()->triggerTestNote(syntPartIdx, note, true);
            }
            else if (state == buttonRelease)
            {
                IE->getSynth()->triggerTestNote(syntPartIdx, note, false);
            }
            break;
        }
        case interfaceButtonUp:
        {
            if (state == buttonPress)
            {
                IE->changeSynthPatchListPos(-1);
            }
            break;
        }
        case interfaceButtonDown:
        {
            if (state == buttonPress)
            {
                IE->changeSynthPatchListPos(1);
            }
            break;
        }
        default: break;
    }
}

static void functSynthListEncoder(int16_t value)
{
    IE->changeSynthPatchListPos(interfaceGlobals.invertValForList ? -value : value);
}

static void functSynthLoadMiniMiddle(uint8_t state)
{
    if (state == buttonPress)
    {
        functSynthConfirmLoad();
    }
}

static void functSynthEditPatch()
{
    sequencer.stopManualNotes();
    IE->mode = mtInstEditModeSynthPatchEdit;
    IE->lightUpPadBoard();
    IE->showSynthPatchEditorScreen();
    IE->setSynthPatchEditorScreenFunct();
}

void cInstrumentEditor::changeSynthModel(int16_t value)
{
    int syntPartIdx = mtProject.values.lastUsedInstrument - SYNTH_INSTRUMENTS_START_IDX;
    assert(syntPartIdx >= 0 && syntPartIdx < kSynthInstrumentsCount);

    int modelMaxValue = (Synth::kSynthTypes.size() - 1);

    int32_t previousSynthType = mtProject.values.synthPart[syntPartIdx].synthType;

    if (IE->rollListOver &&
        mtProject.values.synthPart[syntPartIdx].synthType + value > modelMaxValue)
    {
        mtProject.values.synthPart[syntPartIdx].synthType = 0;
    }
    else
    {
        mtProject.values.synthPart[syntPartIdx].synthType =
          clamp<int>(mtProject.values.synthPart[syntPartIdx].synthType + value, 0, modelMaxValue);
    }

    if (previousSynthType == mtProject.values.synthPart[syntPartIdx].synthType)
    {
        return;
    }

    synth->setInstrumentType(
      syntPartIdx, Synth::kSynthTypes.at(mtProject.values.synthPart[syntPartIdx].synthType));

    changeSynthPolyphony(0);

    showSynthModel(syntPartIdx);

    initCurrentPatch();
    lightUpPadBoard();
    newFileManager.setProjectStructChanged();
}

void cInstrumentEditor::changeSynthPolyphony(int16_t value)
{
    int syntPartIdx = mtProject.values.lastUsedInstrument - SYNTH_INSTRUMENTS_START_IDX;
    assert(syntPartIdx >= 0 && syntPartIdx < kSynthInstrumentsCount);

    mtProject.values.synthPart[syntPartIdx].voiceCount =
      clamp<int>(mtProject.values.synthPart[syntPartIdx].voiceCount + value,
                 0,
                 synth->getMaxVoiceCount(syntPartIdx));

    synth->setVoiceCount(syntPartIdx, mtProject.values.synthPart[syntPartIdx].voiceCount);

    showSynthPolyphony(syntPartIdx);
    titleBar->showCurrentInstrumentOnRightLabel();

    newFileManager.setProjectStructChanged();
}

void cInstrumentEditor::changeSynthGain(int16_t value)
{
    int syntPartIdx = mtProject.values.lastUsedInstrument - SYNTH_INSTRUMENTS_START_IDX;
    assert(syntPartIdx >= 0 && syntPartIdx < kSynthInstrumentsCount);

    mtProject.values.synthPart[syntPartIdx].gainDb =
      clamp<float>(mtProject.values.synthPart[syntPartIdx].gainDb + (value * 0.5),
                   Synth::kGainSilence,
                   Synth::kGainMax);

    setSynthGain(syntPartIdx, mtProject.values.synthPart[syntPartIdx].gainDb);
}

void cInstrumentEditor::changeSynthPanning(int16_t value)
{
    int syntPartIdx = mtProject.values.lastUsedInstrument - SYNTH_INSTRUMENTS_START_IDX;
    assert(syntPartIdx >= 0 && syntPartIdx < kSynthInstrumentsCount);

    mtProject.values.synthPart[syntPartIdx].panning =
      clamp<float>(mtProject.values.synthPart[syntPartIdx].panning + value,
                   -Synth::kPanningMax,
                   Synth::kPanningMax);

    setSynthPanning(syntPartIdx, mtProject.values.synthPart[syntPartIdx].panning);
}

void cInstrumentEditor::setSynthPanning(int synthPartIdx, float value, bool show)
{
    engine.setSynthParam(audioEngine::SynthParam::kPanning,
                         synthPartIdx,
                         value,
                         audioEngine::SynthValuesSource::kInstrument);

    if (show)
    {
        showSynthPanning(synthPartIdx);
    }

    newFileManager.setProjectStructChanged();
}

void cInstrumentEditor::setSynthGain(int synthPartIdx, float value, bool show)
{
    engine.setSynthParam(audioEngine::SynthParam::kVolume,
                         synthPartIdx,
                         value,
                         audioEngine::SynthValuesSource::kInstrument);

    if (show)
    {
        showSynthGain(synthPartIdx);
    }

    newFileManager.setProjectStructChanged();
}

void cInstrumentEditor::changeSynthReverbSend(int16_t value)
{
    uint8_t synthPartIdx = instrumentIdxToSynthIdx(mtProject.values.lastUsedInstrument);
    SynthPartSettings& synthPart = mtProject.values.synthPart[synthPartIdx];

    synthPart.reverbSend = clamp<float>(synthPart.reverbSend + value, 0.0f, 100.0f);

    setSynthReverbSend(synthPartIdx, synthPart.reverbSend);
}

void cInstrumentEditor::changeSynthDelaySend(int16_t value)
{
    uint8_t synthPartIdx = instrumentIdxToSynthIdx(mtProject.values.lastUsedInstrument);
    SynthPartSettings& synthPart = mtProject.values.synthPart[synthPartIdx];
    synthPart.delaySend = clamp<float>(synthPart.delaySend + value, 0.0f, 100.0f);

    setSynthDelaySend(synthPartIdx, synthPart.delaySend);
}

void cInstrumentEditor::setSynthReverbSend(int synthPartIdx, float value, bool show)
{
    engine.setSynthParam(audioEngine::SynthParam::kReverbSend,
                         synthPartIdx,
                         value,
                         audioEngine::SynthValuesSource::kInstrument);

    newFileManager.setProjectStructChanged();

    if (show)
    {
        showSynthReverbSend(synthPartIdx);
    }
}

void cInstrumentEditor::setSynthDelaySend(int synthPartIdx, float value, bool show)
{
    engine.setSynthParam(audioEngine::SynthParam::kDelaySend,
                         synthPartIdx,
                         value,
                         audioEngine::SynthValuesSource::kInstrument);

    newFileManager.setProjectStructChanged();

    if (show)
    {
        showSynthDelaySend(synthPartIdx);
    }
}

void cInstrumentEditor::setDefaultSynthReverb()
{
    uint8_t synthPartIdx = instrumentIdxToSynthIdx(mtProject.values.lastUsedInstrument);
    SynthPartSettings& synthPart = mtProject.values.synthPart[synthPartIdx];

    synthPart.reverbSend = Synth::kSendDefaultValue;

    setSynthReverbSend(synthPartIdx, synthPart.reverbSend);
}

void cInstrumentEditor::setDefaultSynthDelay()
{
    uint8_t synthPartIdx = instrumentIdxToSynthIdx(mtProject.values.lastUsedInstrument);
    SynthPartSettings& synthPart = mtProject.values.synthPart[synthPartIdx];

    synthPart.delaySend = Synth::kSendDefaultValue;

    setSynthDelaySend(synthPartIdx, synthPart.delaySend);
}

[[maybe_unused]] void cInstrumentEditor::setDefaultSynthModel()
{
    int modelMaxValue = (Synth::kSynthTypes.size() - 1);
    changeSynthModel(-modelMaxValue);
}

[[maybe_unused]] void cInstrumentEditor::setDefaultSynthPolyphony()
{
    changeSynthPolyphony(-kSynthMaxVoices);
}

void cInstrumentEditor::setDefaultSynthGain()
{
    int syntPartIdx = mtProject.values.lastUsedInstrument - SYNTH_INSTRUMENTS_START_IDX;
    assert(syntPartIdx >= 0 && syntPartIdx < kSynthInstrumentsCount);

    mtProject.values.synthPart[syntPartIdx].gainDb =
      clamp<float>(Synth::kGainDefault, Synth::kGainSilence, Synth::kGainMax);

    setSynthGain(syntPartIdx, mtProject.values.synthPart[syntPartIdx].gainDb);
}

void cInstrumentEditor::setDefaultSynthPanning()
{
    int syntPartIdx = mtProject.values.lastUsedInstrument - SYNTH_INSTRUMENTS_START_IDX;
    assert(syntPartIdx >= 0 && syntPartIdx < kSynthInstrumentsCount);

    mtProject.values.synthPart[syntPartIdx].panning =
      clamp<float>(Synth::kPanningDefault, -Synth::kPanningMax, Synth::kPanningMax);

    setSynthPanning(syntPartIdx, mtProject.values.synthPart[syntPartIdx].panning);
}

void cInstrumentEditor::changeSynthPatchListPos(int16_t value)
{
    patchListPosition =
      clamp(patchListPosition + value, 0, list2Data.length ? list2Data.length - 1 : 0);

    showSynthPatchList();

    if (list2Data.length == 0)
    {
        return;
    }

    int syntPartIdx = mtProject.values.lastUsedInstrument - SYNTH_INSTRUMENTS_START_IDX;
    assert(syntPartIdx >= 0 && syntPartIdx < kSynthInstrumentsCount);

    bool previewStart = true;

    if (!synth->loadPatch(syntPartIdx, patchListDataName[patchListPosition]))
    {
        previewStart = false;
        mtPopups.show(0, "Error loading patch");
    }
    else
    {
        SynthPartSettings tempSettings = mtProject.values.synthPart[syntPartIdx];
        getSynth()->initSynthPartMacrosValues(syntPartIdx, tempSettings);

        engine.refreshSynthInstrumentParams(tempSettings, syntPartIdx);
    }

    if (buttons->isButtonPressed(interfaceButton4))
    {
        getSynth()->triggerTestNote(syntPartIdx, kSynthPreviewNote1, false);
        if (previewStart)
        {
            getSynth()->triggerTestNote(syntPartIdx, kSynthPreviewNote1, true);
        }
    }

    if (buttons->isButtonPressed(interfaceButton5))
    {
        getSynth()->triggerTestNote(syntPartIdx, kSynthPreviewNote2, false);
        if (previewStart)
        {
            getSynth()->triggerTestNote(syntPartIdx, kSynthPreviewNote2, true);
        }
    }
}

bool cInstrumentEditor::browsePatchList()
{
    int syntPartIdx = mtProject.values.lastUsedInstrument - SYNTH_INSTRUMENTS_START_IDX;
    assert(syntPartIdx >= 0 && syntPartIdx < kSynthInstrumentsCount);

    engine.getSdPlayer()->stop();
    patchfileNamesBuffer = reinterpret_cast<char*>(externalRamScratchBuff);
    patchListDataName =
      reinterpret_cast<char**>(externalRamScratchBuff + kMaxFilesListLength * (kMaxFileNameLen + 1));

    patchListPosition = 0;
    list2Data.start = 0;
    list2Data.linesCount = 14;
    list2Data.lineWidthMax = 0;

    previousPatchName = synth->getPatchName(syntPartIdx);
    patchListPosition = loadPatchList();

    if (list2Data.length == 0)
    {
        return false;
    }

    if (previousPatchName == "" || previousPatchName == Synth::kInitPatchName)
    {
        if (!synth->loadPatch(syntPartIdx, patchListDataName[patchListPosition]))
        {
            mtPopups.show(0, "Error loading patch");
        }
    }
    return true;
}

int cInstrumentEditor::loadPatchList()
{
    assert(patchfileNamesBuffer);
    assert(patchListDataName);
    assert(synth);

    int syntPartIdx = mtProject.values.lastUsedInstrument - SYNTH_INSTRUMENTS_START_IDX;
    assert(syntPartIdx >= 0 && syntPartIdx < kSynthInstrumentsCount);

    memset(patchListDataName, 0, kMaxFilesListLength);

    auto patchList = synth->getPatchList(syntPartIdx);

    int idx = 0;
    int previousPatchIdx = 0;
    char* destHead = patchfileNamesBuffer;
    for (auto& patchName : patchList)
    {
        patchListDataName[idx] = destHead;
        const auto len = patchName.length() + 1;
        polystrncpy(destHead, patchName.data(), len);
        destHead += len;

        if (previousPatchName == patchName)
        {
            previousPatchIdx = idx;
        }

        idx++;
    }

    list2Data.start = previousPatchIdx;
    list2Data.data = patchListDataName;
    list2Data.length = patchList.size();

    return previousPatchIdx;
}

void cInstrumentEditor::revertPatch()
{
    int syntPartIdx = mtProject.values.lastUsedInstrument - SYNTH_INSTRUMENTS_START_IDX;
    assert(syntPartIdx >= 0 && syntPartIdx < kSynthInstrumentsCount);

    synth->loadPatchFromWorkspace(syntPartIdx, previousPatchName);
}

void cInstrumentEditor::initCurrentPatch()
{
    int syntPartIdx = mtProject.values.lastUsedInstrument - SYNTH_INSTRUMENTS_START_IDX;
    assert(syntPartIdx >= 0 && syntPartIdx < kSynthInstrumentsCount);

    IE->getSynth()->initPatch(syntPartIdx);

    IE->getSynth()->initSynthPartMacrosValues(syntPartIdx, mtProject.values.synthPart[syntPartIdx]);
    engine.refreshSynthInstrumentParams(mtProject.values.synthPart[syntPartIdx], syntPartIdx);

    polystrncpy(mtProject.values.synthPart[syntPartIdx].patchName,
                IE->getSynth()->getPatchName(syntPartIdx).c_str(),
                sizeof(SynthPartSettings::patchName));

    newFileManager.setProjectStructChanged();

    mtPopups.show(0, "Init patch loaded");

    IE->showSynthPatchName(syntPartIdx);
}

// ------------------------------------------------------------------------------------------
// ------------------------------       GUI     ---------------------------------------------
// ------------------------------------------------------------------------------------------

void cInstrumentEditor::showSynthParamsScreen()
{
    showTitleBar();
    refreshFramePosition();

    for (uint8_t i = 0; i < 8; i++)
    {
        display.setControlColors(label[i], display.getTheme().activeLabelsColors);
        display.setControlColors(barControl[i], localActiveBarColors);
        display.setRemoveControlStyle(label[i], controlStyleLimitTextWidth);
    }

    display.setRemoveControlStyle(label[6], controlStyleShowBitmap);
    display.setRemoveControlStyle(label[7], controlStyleSpecialFx2);
    display.setRemoveControlStyle(label[6], controlStyleBackground);
    display.setRemoveControlStyle(label[7], controlStyleBackground);

    display.setControlColors(label[6], display.getTheme().activeButtonLabelsColors);
    display.setControlColors(label[7], display.getTheme().activeButtonLabelsColors);
    display.setControlColors(modelListControl, display.getTheme().activeListColors);

    display.setControlText(label[0], "Model");
    display.setControlText(label[1], "Polyphony");
    display.setControlText(label[2], "Gain");
    display.setControlText(label[3], "Panning");
    display.setControlText(label[4], "Reverb send");
    display.setControlText(label[5], "Delay send");
    display.setControlText(label[6], "Load");
    display.setControlText(label[7], "Edit");

    display.setControlText2(label[0], "");
    display.setControlText2(label[1], "");
    display.setControlText2(label[2], "");
    display.setControlText2(label[3], "");
    display.setControlText2(label[4], "");
    display.setControlText2(label[5], "");
    display.setControlText2(label[6], "Patch");
    display.setControlText2(label[7], "Patch");
    display.setControlPosition(label[4], (800 / 8) * 4 + (800 / 16), -1);

    int syntPartIdx = mtProject.values.lastUsedInstrument - SYNTH_INSTRUMENTS_START_IDX;
    assert(syntPartIdx >= 0 && syntPartIdx < kSynthInstrumentsCount);

    showSynthPolyphony(syntPartIdx);
    showSynthGain(syntPartIdx);
    showSynthPanning(syntPartIdx);
    showSynthReverbSend(syntPartIdx);
    showSynthDelaySend(syntPartIdx);
    showSynthPatchName(syntPartIdx);

    display.setControlText(noteLabel, "Patch:");
    display.setControlStyle(noteLabel, controlStyleShow | controlStyleCenterY | controlStyleFont4);
    display.setControlStyle2(noteLabel, controlStyleShow | controlStyleCenterY | controlStyleFont2);
    display.setControlColors(noteLabel, display.getTheme().activeButtonLabelsColors);
    display.setControlValue(noteLabel, 1);
    display.setControlPosition(noteLabel, 625, 45);
    display.setControlSize(noteLabel, 170, 30);

    modelListData.start = mtProject.values.synthPart[syntPartIdx].synthType;
    modelListData.length = Synth::kSynthTypes.size();
    modelListData.linesCount = std::min<uint16_t>(Synth::kSynthTypes.size(), 7);
    modelListData.rowHeight = 50;
    modelListData.params = nullptr;
    modelListData.imageIds = synthModelListImages;
    modelListData.scrollText = true;

    display.setControlData(modelListControl, &modelListData);
    display.setControlPosition(modelListControl, (800 / 8) * 0 + 1, -1);
    display.setControlSize(modelListControl, 800 / 8 - 3, -1);
    display.setControlShow(modelListControl);
    display.refreshControl(modelListControl);

    display.setControlHide(doubleLabelAutomation);
    display.setControlHide(instrumentSynthCCListControl);
    display.setControlHide(instrumentListControl);
    display.setControlHide(filterModeListControl);
    display.setControlHide(blankSpaceCover[0]);
    display.setControlHide(list1Control);
    display.setControlHide(list2Control);
    display.setControlHide(list3Control);
    display.setControlHide(list4Control);
    display.setControlHide(paramsListControl);
    display.setControlHide(imgGridControl);

    display.setControlHide(barControl[0]);
    display.setControlShow(barControl[1]);
    display.setControlShow(barControl[2]);
    display.setControlShow(barControl[3]);
    display.setControlShow(barControl[4]);
    display.setControlShow(barControl[5]);
    display.setControlHide(barControl[6]);
    display.setControlHide(barControl[7]);

    uint32_t shadow = (display.getTheme().bottomScreenShadow > 0 ? controlStyleBottomShadow : 0);
    uint32_t gradient = (display.getTheme().barShadow > 0 ? controlStyleGradient : 0);

    display.setControlStyle(barControl[1],
                            (controlStyleShow | controlStyleValue_0_100 | controlStyleBackground) |
                              shadow | gradient);
    display.setControlStyle(barControl[2],
                            (controlStyleShow | controlStyleValue_0_100 | controlStyleBackground) |
                              shadow | gradient);
    display.setControlStyle(
      barControl[3],
      (controlStyleShow | controlStyleValueLeftRight_100_100 | controlStyleBackground) | shadow |
        gradient);
    display.setControlStyle(barControl[4],
                            (controlStyleShow | controlStyleValue_0_100 | controlStyleBackground) |
                              shadow | gradient);
    display.setControlStyle(barControl[5],
                            (controlStyleShow | controlStyleValue_0_100 | controlStyleBackground) |
                              shadow | gradient);

    display.refreshControl(barControl[1]);
    display.refreshControl(barControl[2]);
    display.refreshControl(barControl[3]);
    display.refreshControl(barControl[4]);
    display.refreshControl(barControl[5]);

    for (uint8_t i = 0; i < 8; i++)
    {
        if (i == 0)
            display.setControlValue(label[i], 0);
        else
            display.setControlValue(label[i], 1);

        display.setControlStyle2(label[i],
                                 controlStyleCenterX | controlStyleCenterY |
                                   (i < 6 ? controlStyleFont2 : controlStyleFont3));
        display.setControlShow(label[i]);
        display.refreshControl(label[i]);
    }

    display.setControlValue(bgLabel, 255);
    display.refreshControl(bgLabel);
    display.setControlShow(bgLabel);

    display.setControlPosition(blankSpaceCover[0], (800 / 8) * 6 + 1, -1);
    display.setControlSize(blankSpaceCover[0], (800 / 8) * 2 - 3, 394);
    display.setControlShow(blankSpaceCover[0]);
    display.refreshControl(blankSpaceCover[0]);

    display.synchronizeRefresh();
}

void cInstrumentEditor::showSynthModel(uint8_t synthPart)
{
    display.setControlValue(modelListControl, mtProject.values.synthPart[synthPart].synthType);
    display.refreshControl(modelListControl);
    titleBar->showCurrentInstrumentOnRightLabel();
}

void cInstrumentEditor::showSynthPolyphony(uint8_t synthPart)
{
    int value = mtProject.values.synthPart[synthPart].voiceCount;
    int valueMax = synth->getMaxVoiceCount(synthPart);

    snprintf(labelText[1], sizeof(labelText[1]), "%d", value > 0 && valueMax > 0 ? value : 0);

    display.setControlText2(label[1], labelText[1]);
    display.refreshControl(label[1]);

    display.setControlValue(barControl[1],
                            value > 0 && valueMax > 0 ? (100 * value / valueMax) : 0);
    display.refreshControl(barControl[1]);
}

void cInstrumentEditor::showSynthGain(uint8_t synthPart)
{
    float value = mtProject.values.synthPart[synthPart].gainDb;

    snprintf(labelText[0], sizeof(labelText[0]), "%0.02f dB", value);

    display.setControlText2(label[2], labelText[0]);
    display.refreshControl(label[2]);

    display.setControlValue(barControl[2],
                            map<int>(value, Synth::kGainSilence, Synth::kGainMax, 0, 100));
    display.refreshControl(barControl[2]);
}

void cInstrumentEditor::showSynthPanning(uint8_t synthPart)
{
    float value = mtProject.values.synthPart[synthPart].panning;

    snprintf(labelText[3], sizeof(labelText[3]), "%0.0f ", value);

    display.setControlText2(label[3], labelText[3]);
    display.refreshControl(label[3]);

    display.setControlValue(barControl[3], value);
    // map<int>(value, -Synth::kPanningMax, Synth::kPanningMax, 0, 100);
    display.refreshControl(barControl[3]);
}

void cInstrumentEditor::showSynthReverbSend(uint8_t syntPartIdx)
{
    float sendInDecybels =
      calcInstrumentSendsDecybelValue(mtProject.values.synthPart[syntPartIdx].reverbSend);

    snprintf(labelText[4], sizeof(labelText[4]), "%0.02f dB", sendInDecybels);

    display.setControlText2(label[4], labelText[4]);
    display.refreshControl(label[4]);

    display.setControlValue(barControl[4], mtProject.values.synthPart[syntPartIdx].reverbSend);
    display.refreshControl(barControl[4]);
}

void cInstrumentEditor::showSynthDelaySend(uint8_t syntPartIdx)
{
    float sendInDecybels =
      calcInstrumentSendsDecybelValue(mtProject.values.synthPart[syntPartIdx].delaySend);

    snprintf(labelText[5], sizeof(labelText[5]), "%0.02f dB", sendInDecybels);

    display.setControlText2(label[5], labelText[5]);
    display.refreshControl(label[5]);

    display.setControlValue(barControl[5], mtProject.values.synthPart[syntPartIdx].delaySend);
    display.refreshControl(barControl[5]);
}

void cInstrumentEditor::showSynthPatchName(uint8_t synthPart)
{
    polystrncpy(
      synthPatchNameText, synth->getPatchName(synthPart).c_str(), sizeof(synthPatchNameText));
    display.setControlText2(noteLabel, synthPatchNameText);
    display.setControlShow(noteLabel);
    display.refreshControl(noteLabel);
}

void cInstrumentEditor::showSynthPatchBrowser()
{
    display.setControlValue(list2Control, patchListPosition);
    display.setControlData(list2Control, &list2Data);
    display.setControlPosition(list2Control, (800 / 8) * 6 + 1, -1);
    display.setControlSize(list2Control, (800 / 8) * 2 - 3, -1);
    display.setControlShow(list2Control);
    display.refreshControl(list2Control);

    display.setControlColors(label[0], display.getTheme().inactiveLabelsColors);
    display.setControlColors(label[1], display.getTheme().inactiveLabelsColors);
    display.setControlColors(label[2], display.getTheme().inactiveLabelsColors);
    display.setControlColors(label[3], display.getTheme().inactiveLabelsColors);
    display.setControlColors(label[4], display.getTheme().inactiveLabelsColors);
    display.setControlColors(label[5], display.getTheme().inactiveLabelsColors);
    display.setControlColors(barControl[1], localInactiveBarColors);
    display.setControlColors(barControl[2], localInactiveBarColors);
    display.setControlColors(barControl[3], localInactiveBarColors);
    display.setControlColors(barControl[4], localInactiveBarColors);
    display.setControlColors(barControl[5], localInactiveBarColors);
    display.setControlColors(modelListControl, display.getTheme().inactiveListColors);
    display.refreshControl(modelListControl);

    if (IE->list2Data.length == 0)
    {
        display.setControlColors(label[4], display.getTheme().inactiveButtonLabelsColors);
        display.setControlColors(label[7], display.getTheme().inactiveButtonLabelsColors);
    }
    else
    {
        display.setControlColors(label[4], display.getTheme().activeButtonLabelsColors);
        display.setControlColors(label[7], display.getTheme().activeButtonLabelsColors);
    }

    display.setControlText(label[4], "Preview");
    display.setControlText(label[5], "");
    display.setControlText(label[6], "Cancel");
    display.setControlText(label[7], "Confirm");
    display.setControlText2(label[4], "");
    display.setControlText2(label[5], "");
    display.setControlValue(label[4], 0);
    display.setControlValue(label[6], 0);
    display.setControlValue(label[7], 0);
    display.setControlPosition(label[4], (800 / 8) * 5, -1);

    display.setControlHide(noteLabel);
    display.setControlHide(blankSpaceCover[0]);

    frameData.placesCount = 1;
    frameData.startPlace = 0;
    frameData.places[0] = &framesPlacesSynth[0][0];
    display.setControlValue(frameControl, 0);
    display.setControlShow(frameControl);
    display.refreshControl(frameControl);

    for (uint8_t i = 1; i < 6; i++)
    {
        display.refreshControl(barControl[i]);
    }

    for (uint8_t i = 0; i < 8; i++)
    {
        display.refreshControl(label[i]);
    }

    display.setControlValue(bgLabel, 222);
    display.refreshControl(bgLabel);

    display.synchronizeRefresh();
}

void cInstrumentEditor::showSynthPatchList()
{
    display.setControlValue(list2Control, patchListPosition);
    display.refreshControl(list2Control);
}

void cInstrumentEditor::showSynthPatchInitPopup()
{
    for (uint8_t i = 0; i < 8; i++)
    {
        display.setControlText(label[i], "");
        display.setControlText2(label[i], "");

        display.refreshControl(label[i]);
        display.setControlShow(label[i]);
    }

    display.setControlValue(label[6], 0);
    display.setControlValue(label[7], 0);
    display.setControlText(label[7], "Yes");
    display.setControlText(label[6], "Cancel");
    display.setControlColors(label[6], display.getTheme().highlightedButtonLabelsColors);
    display.setControlColors(label[7], display.getTheme().highlightedButtonLabelsColors);
    display.setRemoveControlStyle(label[6], controlStyleShowBitmap);
    display.setAddControlStyle(label[6], controlStyleBackground);
    display.setAddControlStyle(label[7], controlStyleBackground);

    display.refreshControl(label[6]);
    display.refreshControl(label[7]);

    display.setControlHide(frameControl);

    mtPopups.showInfoPopup("Are you sure you want to load Init Patch?", nullptr);

    display.setControlValue(bgLabel, 255);
    display.setControlShow(bgLabel);
    display.refreshControl(bgLabel);

    display.synchronizeRefresh();
}

#endif
