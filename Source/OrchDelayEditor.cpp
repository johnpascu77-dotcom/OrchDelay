#include "OrchDelayEditor.h"
#include "OrchDelayBuildInfo.h"
#include "OrchDelayLink.h"

#include <algorithm>
#include <array>

namespace
{
    // House color palette, matching OrchGate's own editor conventions.
    const auto kBackground = juce::Colour::fromRGB (18, 22, 28);
    const auto kThumb = juce::Colour::fromRGB (95, 220, 140);
    const auto kTrack = juce::Colour::fromRGB (95, 200, 245);
    const auto kBoxBackground = juce::Colour::fromRGB (28, 36, 46);
    const auto kOutline = juce::Colour::fromRGB (120, 135, 150);
    const auto kMuted = juce::Colour::fromRGB (140, 160, 180);
}

OrchDelayAudioProcessorEditor::OrchDelayAudioProcessorEditor (OrchDelayAudioProcessor& p)
    : AudioProcessorEditor (&p), audioProcessor (p)
{
    // Every one of SS39-SS43 (Hub Presets, Ignore Keyswitches, Reset All
    // Instances, Output Channel override, Monophonic Capture) bumped this
    // window's own fixed height a bit further, on the theory that
    // resizability (see below) covered it - it didn't, once the total
    // genuinely exceeded a real screen's usable height with no way to see
    // the rest (confirmed live, 2026-09-28). SS44 fixes this the durable way
    // instead: the main panel now lives in mainPanelContent, a fixed-size
    // scrollable canvas behind mainPanelViewport (see that member's own doc
    // comment in the header, and resized()'s own comment where it sets
    // mainPanelContent's size) - so the OUTER window itself only ever needs
    // to be a comfortable, real-screen-friendly size, never the content's
    // own full height. Still genuinely resizable within reason (a host
    // panel, in particular, can render less space than even this).
    setResizable (true, true);
    setResizeLimits (900, 500, 1400, 1000);
    setSize (1040, 760);

    titleLabel.setText ("OrchDelay", juce::dontSendNotification);
    titleLabel.setJustificationType (juce::Justification::centred);
    titleLabel.setColour (juce::Label::textColourId, juce::Colours::white);
    titleLabel.setFont (juce::FontOptions (30.0f, juce::Font::bold));
    addAndMakeVisible (titleLabel);

    subtitleLabel.setText ("Responsorial Delay", juce::dontSendNotification);
    subtitleLabel.setJustificationType (juce::Justification::centred);
    subtitleLabel.setColour (juce::Label::textColourId, juce::Colours::white);
    subtitleLabel.setFont (juce::FontOptions (15.0f));
    addAndMakeVisible (subtitleLabel);

    // orchDelayBuildTimestamp is regenerated on every single build (see
    // cmake/GenerateOrchDelayBuildInfo.cmake) - a fresh timestamp, not a
    // hand-maintained tag, is what actually answers "is this the build I
    // just installed."
    buildLabel.setText (juce::String ("Build: ") + orchDelayBuildTimestamp, juce::dontSendNotification);
    buildLabel.setJustificationType (juce::Justification::centred);
    buildLabel.setColour (juce::Label::textColourId, kMuted);
    buildLabel.setFont (juce::FontOptions (12.0f));
    addAndMakeVisible (buildLabel);

    bypassButton.setButtonText ("Bypass");
    bypassButton.setColour (juce::ToggleButton::textColourId, juce::Colours::white);
    addAndMakeVisible (bypassButton);

    captureModeLabel.setText ("Capture Mode", juce::dontSendNotification);
    captureModeLabel.setJustificationType (juce::Justification::centredLeft);
    captureModeLabel.setColour (juce::Label::textColourId, juce::Colours::white);
    captureModeLabel.setFont (juce::FontOptions (14.0f));
    addAndMakeVisible (captureModeLabel);
    captureModeBox.addItem ("Replace", 1);
    captureModeBox.addItem ("Overlay", 2);
    captureModeBox.addItem ("Duck", 3);
    captureModeBox.setColour (juce::ComboBox::backgroundColourId, kBoxBackground);
    captureModeBox.setColour (juce::ComboBox::textColourId, juce::Colours::white);
    captureModeBox.setColour (juce::ComboBox::outlineColourId, kOutline);
    addAndMakeVisible (captureModeBox);

    // Docs SS40 - see OrchDelayProcessor.cpp's own doc comment on
    // "ignoreKeyswitches" for why this exists. Set up via the shared
    // setupLabel/setupSlider helpers just below, once they're defined -
    // deferred to right after Autonomous Fire's own setup, alongside the
    // rest of the simple label+slider rows.

    auto setupLabel = [] (juce::Label& label, const juce::String& text)
    {
        label.setText (text, juce::dontSendNotification);
        label.setJustificationType (juce::Justification::centredLeft);
        label.setColour (juce::Label::textColourId, juce::Colours::white);
        label.setFont (juce::FontOptions (14.0f));
    };

    auto setupSlider = [this] (juce::Slider& slider)
    {
        slider.setSliderStyle (juce::Slider::LinearHorizontal);
        slider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 72, 24);
        slider.setColour (juce::Slider::thumbColourId, kThumb);
        slider.setColour (juce::Slider::trackColourId, kTrack);
        slider.setColour (juce::Slider::textBoxTextColourId, juce::Colours::white);
        slider.setColour (juce::Slider::textBoxBackgroundColourId, kBoxBackground);
        addAndMakeVisible (slider);
    };

    setupLabel (holdBarsLabel, "Hold Bars");
    addAndMakeVisible (holdBarsLabel);
    setupSlider (holdBarsSlider);
    holdBarsRandomButton.setButtonText ("Random");
    holdBarsRandomButton.setColour (juce::ToggleButton::textColourId, juce::Colours::white);
    addAndMakeVisible (holdBarsRandomButton);
    setupRandomRangeBinding (holdBarsRangeSlider, holdBarsSlider, holdBarsRandomButton, holdBarsLabel,
                             false, "holdBarsRandomMin", "holdBarsRandomMax");

    setupLabel (overlapModeLabel, "Overlap Mode");
    addAndMakeVisible (overlapModeLabel);
    overlapModeBox.addItem ("Overlap", 1);
    overlapModeBox.addItem ("Wait", 2);
    overlapModeBox.addItem ("Skip", 3);
    overlapModeBox.setColour (juce::ComboBox::backgroundColourId, kBoxBackground);
    overlapModeBox.setColour (juce::ComboBox::textColourId, juce::Colours::white);
    overlapModeBox.setColour (juce::ComboBox::outlineColourId, kOutline);
    addAndMakeVisible (overlapModeBox);

    setupLabel (phraseGapLabel, "Phrase Gap (beats)");
    addAndMakeVisible (phraseGapLabel);
    setupSlider (phraseGapSlider);

    setupLabel (minimumInterestLabel, "Minimum Interest");
    addAndMakeVisible (minimumInterestLabel);
    setupSlider (minimumInterestSlider);

    setupLabel (callbackProbabilityLabel, "Callback Probability");
    addAndMakeVisible (callbackProbabilityLabel);
    setupSlider (callbackProbabilitySlider);

    setupLabel (autonomousFireLabel, "Autonomous Fire (bars)");
    addAndMakeVisible (autonomousFireLabel);
    setupSlider (autonomousFireSlider);

    // Docs SS40: mirror of OrchGate's own "Pass Keyswitches" - here the
    // keyswitch range is never captured/fired/broadcast in the first place,
    // rather than always let through regardless of gate state.
    ignoreKeyswitchesButton.setButtonText ("Ignore Keyswitches");
    ignoreKeyswitchesButton.setColour (juce::ToggleButton::textColourId, juce::Colours::white);
    addAndMakeVisible (ignoreKeyswitchesButton);
    setupLabel (ksIgnoreMinLabel, "KS Ignore Min");
    addAndMakeVisible (ksIgnoreMinLabel);
    setupSlider (ksIgnoreMinSlider);
    setupLabel (ksIgnoreMaxLabel, "KS Ignore Max");
    addAndMakeVisible (ksIgnoreMaxLabel);
    setupSlider (ksIgnoreMaxSlider);

    // Docs SS43: see monophonicCapture's own doc comment in
    // createParameterLayout for the live-rig bug this fixes.
    monophonicCaptureButton.setButtonText ("Monophonic Capture");
    monophonicCaptureButton.setColour (juce::ToggleButton::textColourId, juce::Colours::white);
    addAndMakeVisible (monophonicCaptureButton);

    // Multi-bank memory (see Docs SS25): Capture Bank is where newly-closed
    // phrases get remembered; Active Bank is where Callback Probability and
    // Autonomous Fire draw from. Deliberately two independent selectors, not
    // one shared knob - build up new material in one bank while a different
    // one keeps playing, then switch Active Bank over when ready.
    setupLabel (captureBankLabel, "Capture Bank");
    addAndMakeVisible (captureBankLabel);
    captureBankBox.addItem ("A", 1);
    captureBankBox.addItem ("B", 2);
    captureBankBox.addItem ("C", 3);
    captureBankBox.setColour (juce::ComboBox::backgroundColourId, kBoxBackground);
    captureBankBox.setColour (juce::ComboBox::textColourId, juce::Colours::white);
    captureBankBox.setColour (juce::ComboBox::outlineColourId, kOutline);
    addAndMakeVisible (captureBankBox);

    // "Clear Bank" - clears whichever bank Capture Bank is CURRENTLY set to,
    // so a fresh idea/motive can start without old material bleeding back in
    // via Callback/Autonomous Fire (see OrchDelayProcessor::requestClearCaptureBank).
    clearBankButton.setButtonText ("Clear Bank");
    clearBankButton.setColour (juce::TextButton::buttonColourId, kBoxBackground);
    clearBankButton.setColour (juce::TextButton::textColourOffId, juce::Colours::white);
    clearBankButton.onClick = [this] { audioProcessor.requestClearCaptureBank(); };
    addAndMakeVisible (clearBankButton);

    setupLabel (activeBankLabel, "Active Bank");
    addAndMakeVisible (activeBankLabel);
    activeBankBox.addItem ("A", 1);
    activeBankBox.addItem ("B", 2);
    activeBankBox.addItem ("C", 3);
    activeBankBox.addItem ("Remote", 4);
    activeBankBox.setColour (juce::ComboBox::backgroundColourId, kBoxBackground);
    activeBankBox.setColour (juce::ComboBox::textColourId, juce::Colours::white);
    activeBankBox.setColour (juce::ComboBox::outlineColourId, kOutline);
    addAndMakeVisible (activeBankBox);

    // Biases Callback Probability's and Autonomous Fire's pool draw toward
    // more recently-captured entries within Active Bank - 0% (default) is
    // today's original flat/uniform pick, unchanged (see Docs SS26).
    setupLabel (recencyBiasLabel, "Recency Bias");
    addAndMakeVisible (recencyBiasLabel);
    setupSlider (recencyBiasSlider);

    setupLabel (restlessnessLabel, "Restlessness");
    addAndMakeVisible (restlessnessLabel);
    setupSlider (restlessnessSlider);
    contentAwareWeightingButton.setButtonText ("Content-Aware");
    contentAwareWeightingButton.setColour (juce::ToggleButton::textColourId, juce::Colours::white);
    addAndMakeVisible (contentAwareWeightingButton);

    setupLabel (transformLabel, "Transform");
    addAndMakeVisible (transformLabel);
    transformBox.addItem ("Follow Restlessness", 1);
    transformBox.addItem ("None", 2);
    transformBox.addItem ("Transpose", 3);
    transformBox.addItem ("Retrograde", 4);
    transformBox.addItem ("Inversion", 5);
    transformBox.addItem ("Rotation", 6);
    transformBox.addItem ("Length", 7);
    transformBox.addItem ("M7", 8);
    transformBox.addItem ("Stretch", 9);
    transformBox.addItem ("Interval", 10);
    transformBox.setColour (juce::ComboBox::backgroundColourId, kBoxBackground);
    transformBox.setColour (juce::ComboBox::textColourId, juce::Colours::white);
    transformBox.setColour (juce::ComboBox::outlineColourId, kOutline);
    addAndMakeVisible (transformBox);

    setupLabel (transposeLabel, "Transpose (semitones)");
    addAndMakeVisible (transposeLabel);
    setupSlider (transposeSlider);

    // "Random" - when on, Transpose draws from its own dedicated min/max
    // range instead of a fixed amount (see
    // odly::resolveRandomTransposeSemitones, Docs SS32).
    transposeRandomButton.setButtonText ("Random");
    transposeRandomButton.setColour (juce::ToggleButton::textColourId, juce::Colours::white);
    addAndMakeVisible (transposeRandomButton);
    setupRandomRangeBinding (transposeRangeSlider, transposeSlider, transposeRandomButton, transposeLabel,
                             false, "transposeRandomMin", "transposeRandomMax");

    setupLabel (rotationLabel, "Rotation (steps)");
    addAndMakeVisible (rotationLabel);
    setupSlider (rotationSlider);
    rotationRandomButton.setButtonText ("Random");
    rotationRandomButton.setColour (juce::ToggleButton::textColourId, juce::Colours::white);
    addAndMakeVisible (rotationRandomButton);
    setupRandomRangeBinding (rotationRangeSlider, rotationSlider, rotationRandomButton, rotationLabel,
                             false, "rotationRandomMin", "rotationRandomMax");

    setupLabel (lengthLabel, "Length (%)");
    addAndMakeVisible (lengthLabel);
    setupSlider (lengthSlider);
    lengthRandomButton.setButtonText ("Random");
    lengthRandomButton.setColour (juce::ToggleButton::textColourId, juce::Colours::white);
    addAndMakeVisible (lengthRandomButton);
    setupRandomRangeBinding (lengthRangeSlider, lengthSlider, lengthRandomButton, lengthLabel,
                             true, "lengthRandomMin", "lengthRandomMax");

    setupLabel (stretchLabel, "Stretch (%)");
    addAndMakeVisible (stretchLabel);
    setupSlider (stretchSlider);
    stretchRandomButton.setButtonText ("Random");
    stretchRandomButton.setColour (juce::ToggleButton::textColourId, juce::Colours::white);
    addAndMakeVisible (stretchRandomButton);
    stretchQuantizedButton.setButtonText ("Quantize");
    stretchQuantizedButton.setColour (juce::ToggleButton::textColourId, juce::Colours::white);
    addAndMakeVisible (stretchQuantizedButton);
    setupRandomRangeBinding (stretchRangeSlider, stretchSlider, stretchRandomButton, stretchLabel,
                             true, "stretchRandomMin", "stretchRandomMax");

    // Quantize-aware handle snapping (Docs SS36) - overrides the generic
    // binding's own onValueChange set up just above, rather than teaching
    // setupRandomRangeBinding itself about Quantize (a Stretch-only concept
    // none of the other 5 Random-range parameters share). When Quantize is
    // on, the actual Random draw only ever produces one of
    // odly::quantizedStretchRatios() regardless of where the handles sit
    // (see resolveRandomQuantizedStretchPercent) - a handle resting "near"
    // a legal value rather than exactly on it was cosmetically misleading
    // about what the range genuinely contains, and precisely dragging to an
    // exact percentage by feel alone is fiddly. Snaps continuously while
    // dragging (every onValueChange, not just on release) for an actual
    // magnetic-snap feel rather than a jarring teleport on mouse-up.
    {
        auto& state = audioProcessor.getParameters();
        auto* minParam = dynamic_cast<juce::RangedAudioParameter*> (state.getParameter ("stretchRandomMin"));
        auto* maxParam = dynamic_cast<juce::RangedAudioParameter*> (state.getParameter ("stretchRandomMax"));
        stretchRangeSlider.onValueChange = [this, minParam, maxParam]
        {
            float lo = static_cast<float> (stretchRangeSlider.getMinValue());
            float hi = static_cast<float> (stretchRangeSlider.getMaxValue());

            if (stretchQuantizedButton.getToggleState())
            {
                lo = odly::snapToQuantizedStretch (lo);
                hi = odly::snapToQuantizedStretch (hi);
                stretchRangeSlider.setMinAndMaxValues (lo, hi, juce::dontSendNotification);
            }

            if (minParam != nullptr)
                minParam->setValueNotifyingHost (minParam->convertTo0to1 (lo));
            if (maxParam != nullptr)
                maxParam->setValueNotifyingHost (maxParam->convertTo0to1 (hi));
        };
    }

    setupLabel (intervalLabel, "Interval Scale (%)");
    addAndMakeVisible (intervalLabel);
    setupSlider (intervalSlider);
    intervalRandomButton.setButtonText ("Random");
    intervalRandomButton.setColour (juce::ToggleButton::textColourId, juce::Colours::white);
    addAndMakeVisible (intervalRandomButton);
    setupRandomRangeBinding (intervalRangeSlider, intervalSlider, intervalRandomButton, intervalLabel,
                             true, "intervalRandomMin", "intervalRandomMax");

    // Cross-instance phrase broadcast (see OrchDelayLink / Docs SS27): lets
    // one instance's captured phrases feed another instance's Remote bank
    // directly, no MIDI cable needed. Exactly ONE instance in the rig should
    // have "Broadcast Hub" on.
    setupLabel (linkHubLabel, "Broadcast Hub");
    addAndMakeVisible (linkHubLabel);
    linkHubButton.setButtonText ("Hub");
    linkHubButton.setColour (juce::ToggleButton::textColourId, juce::Colours::white);
    addAndMakeVisible (linkHubButton);

    // "Clear Remote" - always targets the Remote bank regardless of Capture
    // Bank's own selection (see requestClearRemoteBank's own doc comment).
    clearRemoteButton.setButtonText ("Clear Remote");
    clearRemoteButton.setColour (juce::TextButton::buttonColourId, kBoxBackground);
    clearRemoteButton.setColour (juce::TextButton::textColourOffId, juce::Colours::white);
    clearRemoteButton.onClick = [this] { audioProcessor.requestClearRemoteBank(); };
    addAndMakeVisible (clearRemoteButton);

    setupLabel (broadcastChannelLabel, "Broadcast Channel");
    addAndMakeVisible (broadcastChannelLabel);
    setupSlider (broadcastChannelSlider);

    setupLabel (listenChannelLabel, "Listen Channel");
    addAndMakeVisible (listenChannelLabel);
    setupSlider (listenChannelSlider);

    setupLabel (outputChannelLabel, "Output Channel (0=auto)");
    addAndMakeVisible (outputChannelLabel);
    setupSlider (outputChannelSlider);

    relayEnabledButton.setButtonText ("Relay Remote Material");
    relayEnabledButton.setColour (juce::ToggleButton::textColourId, juce::Colours::white);
    addAndMakeVisible (relayEnabledButton);

    setupLabel (instanceSeedLabel, "Instance Seed");
    addAndMakeVisible (instanceSeedLabel);
    setupSlider (instanceSeedSlider);

    randomizeSeedButton.setButtonText ("Randomize");
    randomizeSeedButton.setColour (juce::TextButton::buttonColourId, kBoxBackground);
    randomizeSeedButton.setColour (juce::TextButton::textColourOffId, juce::Colours::white);
    randomizeSeedButton.onClick = [this] { audioProcessor.randomizeInstanceSeed(); };
    addAndMakeVisible (randomizeSeedButton);

    // Free-text identity (Docs SS31) - not an APVTS parameter, see
    // OrchDelayProcessor::getInstanceLabelForUi's own doc comment. Written
    // through on focus loss / Return, matching OrchCapture's own free-text
    // field convention rather than on every keystroke.
    setupLabel (instanceLabelLabel, "Instance Label");
    addAndMakeVisible (instanceLabelLabel);
    instanceLabelEditor.setText (audioProcessor.getInstanceLabelForUi(), juce::dontSendNotification);
    instanceLabelEditor.setColour (juce::TextEditor::backgroundColourId, kBoxBackground);
    instanceLabelEditor.setColour (juce::TextEditor::textColourId, juce::Colours::white);
    instanceLabelEditor.setColour (juce::TextEditor::outlineColourId, kOutline);
    instanceLabelEditor.setTextToShowWhenEmpty ("e.g. Violins", kMuted);
    auto commitLabel = [this]
    {
        audioProcessor.setInstanceLabel (instanceLabelEditor.getText());

        // The label isn't an APVTS parameter, so nothing here raises the
        // host's own "project is dirty" flag by itself. Without this,
        // Bitwig was observed serialising a stale getStateInformation()
        // snapshot from load time on Ctrl+S - the label changed in memory
        // and stayed correct all session, but a real save->quit->reopen
        // cycle silently reverted to the last state the host actually
        // asked for. restartComponent (VST3) / updateHostDisplay (JUCE)
        // is the standard hook for "non-parameter state changed, please
        // treat me as dirty and re-fetch before you persist."
        audioProcessor.updateHostDisplay();
    };
    instanceLabelEditor.onFocusLost = commitLabel;
    instanceLabelEditor.onReturnKey = commitLabel;
    addAndMakeVisible (instanceLabelEditor);

    // Connection Matrix tab (Docs SS31) - see this file's own header comment
    // on ConnectionMatrixView for why it's only shown when Hub is checked.
    // Visibility/enablement is driven from timerCallback, not here - it
    // tracks linkHubButton's live toggle state, which can change any time
    // after construction.
    matrixTabButton.setButtonText ("Connection Matrix");
    matrixTabButton.setColour (juce::TextButton::buttonColourId, kBoxBackground);
    matrixTabButton.setColour (juce::TextButton::textColourOffId, juce::Colours::white);
    matrixTabButton.setClickingTogglesState (true);
    matrixTabButton.onClick = [this] { setShowingMatrix (matrixTabButton.getToggleState()); };
    addChildComponent (matrixTabButton);   // starts hidden - see timerCallback

    matrixViewport.setViewedComponent (&matrixView, false);   // false: matrixView is a plain member, not owned by the viewport
    matrixViewport.setScrollBarsShown (true, true);
    addChildComponent (matrixViewport);   // starts hidden - shown via setShowingMatrix

    // SS44: see mainPanelContent's own doc comment in the header. Same
    // Viewport pattern as matrixViewport just above, always visible (unlike
    // matrixViewport, the main panel is the default view on construction).
    mainPanelViewport.setViewedComponent (&mainPanelContent, false);
    mainPanelViewport.setScrollBarsShown (true, false);
    addAndMakeVisible (mainPanelViewport);

    // Click-to-wire (Docs SS33): sourceIdx/destIdx are positions in the
    // SAME row list matrixView just rendered from (getRows()), which is
    // always the most recent snapshot timerCallback built - never stale by
    // more than one tick, since the matrix only refreshes while visible and
    // a click can only happen while it's visible and on-screen.
    matrixView.onCellClicked = [this] (int sourceIdx, int destIdx)
    {
        const auto& rows = matrixView.getRows();
        if (sourceIdx < 0 || destIdx < 0
            || sourceIdx >= static_cast<int> (rows.size()) || destIdx >= static_cast<int> (rows.size()))
            return;

        const auto& source = rows[static_cast<size_t> (sourceIdx)];
        const auto& dest = rows[static_cast<size_t> (destIdx)];

        // Already connected (this exact source feeds this exact
        // destination) - click again to disconnect. Otherwise connect,
        // which naturally supersedes whatever the destination was
        // PREVIOUSLY listening to (Listen Channel is single-valued) with no
        // separate action needed - that old cell just stops being lit.
        const bool alreadyConnected = source.broadcastChannel > 0 && source.broadcastChannel == dest.listenChannel;
        const int newChannel = alreadyConnected ? 0 : source.broadcastChannel;

        if (dest.isSelf)
        {
            if (auto* param = dynamic_cast<juce::RangedAudioParameter*> (audioProcessor.getParameters().getParameter ("listenChannel")))
                param->setValueNotifyingHost (param->convertTo0to1 (static_cast<float> (newChannel)));
        }
        else
        {
            audioProcessor.getLink().sendSetListenChannel (dest.connectionId, newChannel);
        }
    };

    // Hub-pushed presets (SS39) - see this file's own header comment on
    // pendingPresetEntries for why Save/Load only ever fill a hold buffer
    // and Send is the one action that actually touches the rest of the rig.
    // Visibility driven from timerCallback alongside matrixTabButton - only
    // the hub ever has a live table of other instances to capture from.
    presetSectionLabel.setText ("Hub Presets", juce::dontSendNotification);
    presetSectionLabel.setColour (juce::Label::textColourId, juce::Colours::white);
    presetSectionLabel.setFont (juce::FontOptions (13.0f, juce::Font::bold));
    addChildComponent (presetSectionLabel);

    savePresetButton.setButtonText ("Save Preset...");
    savePresetButton.setColour (juce::TextButton::buttonColourId, kBoxBackground);
    savePresetButton.setColour (juce::TextButton::textColourOffId, juce::Colours::white);
    savePresetButton.onClick = [this]
    {
        capturePresetToBuffer();

        presetFileChooser = std::make_unique<juce::FileChooser> (
            "Save routing preset...",
            juce::File::getSpecialLocation (juce::File::userDocumentsDirectory),
            "*.json");

        presetFileChooser->launchAsync (juce::FileBrowserComponent::saveMode
                                             | juce::FileBrowserComponent::warnAboutOverwriting,
            [this] (const juce::FileChooser& chooser)
            {
                const auto file = chooser.getResult();
                if (file != juce::File())
                    savePresetToFile (file);
            });
    };
    addChildComponent (savePresetButton);

    loadPresetButton.setButtonText ("Load Preset...");
    loadPresetButton.setColour (juce::TextButton::buttonColourId, kBoxBackground);
    loadPresetButton.setColour (juce::TextButton::textColourOffId, juce::Colours::white);
    loadPresetButton.onClick = [this]
    {
        presetFileChooser = std::make_unique<juce::FileChooser> (
            "Load routing preset...",
            juce::File::getSpecialLocation (juce::File::userDocumentsDirectory),
            "*.json");

        presetFileChooser->launchAsync (juce::FileBrowserComponent::openMode,
            [this] (const juce::FileChooser& chooser)
            {
                const auto file = chooser.getResult();
                if (file != juce::File())
                    loadPresetFromFile (file);
            });
    };
    addChildComponent (loadPresetButton);

    sendPresetButton.setButtonText ("Send Preset to Rig");
    sendPresetButton.setColour (juce::TextButton::buttonColourId, kBoxBackground);
    sendPresetButton.setColour (juce::TextButton::textColourOffId, juce::Colours::white);
    sendPresetButton.onClick = [this] { sendPendingPreset(); };
    addChildComponent (sendPresetButton);

    // Hub-pushed full reset (SS41) - see this file's own header comment on
    // resetAllButton. Coloured as a warning (unlike the routing-only preset
    // buttons above) since one click wipes every connected instance's
    // musical memory at once, with no per-instance undo - a confirm dialog
    // is the one bit of friction worth keeping even though the whole point
    // of this button is removing the OTHER, far more tedious kind (visiting
    // every instance by hand).
    resetAllButton.setButtonText ("Reset All Instances");
    resetAllButton.setColour (juce::TextButton::buttonColourId, juce::Colours::darkred);
    resetAllButton.setColour (juce::TextButton::textColourOffId, juce::Colours::white);
    resetAllButton.onClick = [this]
    {
        juce::NativeMessageBox::showOkCancelBox (juce::MessageBoxIconType::WarningIcon,
            "Reset All Instances",
            "Clear every OrchDelay instance's captured and relayed phrase memory across the whole rig?\n\n"
            "This cannot be undone.",
            this,
            juce::ModalCallbackFunction::create ([this] (int result)
            {
                if (result != 0)
                    audioProcessor.getLink().sendResetAll();
            }));
    };
    addChildComponent (resetAllButton);

    presetStatusLabel.setJustificationType (juce::Justification::centredLeft);
    presetStatusLabel.setColour (juce::Label::textColourId, kMuted);
    presetStatusLabel.setFont (juce::FontOptions (11.0f));
    presetStatusLabel.setText ("No preset captured yet - Save or Load one.", juce::dontSendNotification);
    addChildComponent (presetStatusLabel);

    // SS45: Hub "General Parameters" - see this file's own header comment on
    // shapeSectionLabel. Plain sliders/toggles (no APVTS attachment - these
    // represent a value to SEND, not this instance's own setting), default-
    // initialised to the same defaults each underlying parameter itself
    // defaults to, so an untouched template starts as a musically-neutral
    // no-op rather than an arbitrary number.
    setupLabel (shapeSectionLabel, "General Parameters (sent to whole rig)");
    shapeSectionLabel.setFont (juce::FontOptions (13.0f, juce::Font::bold));
    addChildComponent (shapeSectionLabel);

    setupLabel (shapeHoldBarsLabel, "Hold Bars");
    addChildComponent (shapeHoldBarsLabel);
    setupSlider (shapeHoldBarsSlider);
    shapeHoldBarsSlider.setRange (0.0, 16.0, 1.0);
    shapeHoldBarsSlider.setValue (4.0, juce::dontSendNotification);
    addChildComponent (shapeHoldBarsSlider);

    setupLabel (shapeAutonomousFireLabel, "Autonomous Fire (bars)");
    addChildComponent (shapeAutonomousFireLabel);
    setupSlider (shapeAutonomousFireSlider);
    shapeAutonomousFireSlider.setRange (0.0, 16.0, 1.0);
    shapeAutonomousFireSlider.setValue (0.0, juce::dontSendNotification);
    addChildComponent (shapeAutonomousFireSlider);

    setupLabel (shapePhraseGapLabel, "Phrase Gap (beats)");
    addChildComponent (shapePhraseGapLabel);
    setupSlider (shapePhraseGapSlider);
    shapePhraseGapSlider.setRange (0.25, 8.0, 0.25);
    shapePhraseGapSlider.setValue (1.0, juce::dontSendNotification);
    addChildComponent (shapePhraseGapSlider);

    setupLabel (shapeMinimumInterestLabel, "Minimum Interest");
    addChildComponent (shapeMinimumInterestLabel);
    setupSlider (shapeMinimumInterestSlider);
    shapeMinimumInterestSlider.setRange (0.0, 100.0, 1.0);
    shapeMinimumInterestSlider.setValue (0.0, juce::dontSendNotification);
    addChildComponent (shapeMinimumInterestSlider);

    setupLabel (shapeCallbackProbabilityLabel, "Callback Probability");
    addChildComponent (shapeCallbackProbabilityLabel);
    setupSlider (shapeCallbackProbabilitySlider);
    shapeCallbackProbabilitySlider.setRange (0.0, 100.0, 1.0);
    shapeCallbackProbabilitySlider.setValue (0.0, juce::dontSendNotification);
    addChildComponent (shapeCallbackProbabilitySlider);

    shapeMonophonicCaptureButton.setButtonText ("Monophonic Capture");
    shapeMonophonicCaptureButton.setColour (juce::ToggleButton::textColourId, juce::Colours::white);
    addChildComponent (shapeMonophonicCaptureButton);

    // Deliberately no KS Ignore Min/Max controls here - see ShapeEntry's own
    // doc comment in OrchDelayLink.h for the live-rig bug this avoids (a
    // uniform push clobbered per-instrument hand-tuned values). Each
    // instance's own Ignore Keyswitches range stays under that instance's
    // own panel, untouched by this section.
    shapeIgnoreKeyswitchesButton.setButtonText ("Ignore Keyswitches");
    shapeIgnoreKeyswitchesButton.setColour (juce::ToggleButton::textColourId, juce::Colours::white);
    shapeIgnoreKeyswitchesButton.setToggleState (true, juce::dontSendNotification);
    addChildComponent (shapeIgnoreKeyswitchesButton);

    sendShapeButton.setButtonText ("Send to Rig");
    sendShapeButton.setColour (juce::TextButton::buttonColourId, kBoxBackground);
    sendShapeButton.setColour (juce::TextButton::textColourOffId, juce::Colours::white);
    sendShapeButton.onClick = [this] { audioProcessor.getLink().sendShapeToRig (buildShapeFromUi(), false); };
    addChildComponent (sendShapeButton);

    // Only the 5 numeric fields jitter per-instance (see jitterShapeEntry's
    // own doc comment in OrchDelayLink.cpp) - no confirm dialog, unlike
    // Reset All Instances: this only nudges musical timing/character within
    // sensible spans around whatever the template already says, nothing here
    // is destructive or hard to walk back (just click Send to Rig again).
    randomizeSendShapeButton.setButtonText ("Randomize & Send");
    randomizeSendShapeButton.setColour (juce::TextButton::buttonColourId, kBoxBackground);
    randomizeSendShapeButton.setColour (juce::TextButton::textColourOffId, juce::Colours::white);
    randomizeSendShapeButton.onClick = [this] { audioProcessor.getLink().sendShapeToRig (buildShapeFromUi(), true); };
    addChildComponent (randomizeSendShapeButton);

    statusLabel.setJustificationType (juce::Justification::centred);
    statusLabel.setColour (juce::Label::textColourId, kMuted);
    statusLabel.setFont (juce::FontOptions (11.0f));
    addAndMakeVisible (statusLabel);

    auto& state = audioProcessor.getParameters();
    bypassAttachment = std::make_unique<ButtonAttachment> (state, "bypass", bypassButton);
    captureModeAttachment = std::make_unique<ComboBoxAttachment> (state, "captureMode", captureModeBox);
    holdBarsAttachment = std::make_unique<SliderAttachment> (state, "holdBars", holdBarsSlider);
    holdBarsRandomAttachment = std::make_unique<ButtonAttachment> (state, "holdBarsRandom", holdBarsRandomButton);
    overlapModeAttachment = std::make_unique<ComboBoxAttachment> (state, "overlapMode", overlapModeBox);
    phraseGapAttachment = std::make_unique<SliderAttachment> (state, "phraseGapBeats", phraseGapSlider);
    minimumInterestAttachment = std::make_unique<SliderAttachment> (state, "minimumInterest", minimumInterestSlider);
    callbackProbabilityAttachment = std::make_unique<SliderAttachment> (state, "callbackProbability", callbackProbabilitySlider);
    autonomousFireAttachment = std::make_unique<SliderAttachment> (state, "autonomousFireBars", autonomousFireSlider);
    ignoreKeyswitchesAttachment = std::make_unique<ButtonAttachment> (state, "ignoreKeyswitches", ignoreKeyswitchesButton);
    ksIgnoreMinAttachment = std::make_unique<SliderAttachment> (state, "ksIgnoreMin", ksIgnoreMinSlider);
    ksIgnoreMaxAttachment = std::make_unique<SliderAttachment> (state, "ksIgnoreMax", ksIgnoreMaxSlider);
    monophonicCaptureAttachment = std::make_unique<ButtonAttachment> (state, "monophonicCapture", monophonicCaptureButton);
    captureBankAttachment = std::make_unique<ComboBoxAttachment> (state, "captureBank", captureBankBox);
    activeBankAttachment = std::make_unique<ComboBoxAttachment> (state, "activeBank", activeBankBox);
    recencyBiasAttachment = std::make_unique<SliderAttachment> (state, "recencyBias", recencyBiasSlider);
    restlessnessAttachment = std::make_unique<SliderAttachment> (state, "restlessness", restlessnessSlider);
    contentAwareWeightingAttachment = std::make_unique<ButtonAttachment> (state, "contentAwareWeighting", contentAwareWeightingButton);
    transformAttachment = std::make_unique<ComboBoxAttachment> (state, "manualTransform", transformBox);
    transposeAttachment = std::make_unique<SliderAttachment> (state, "transposeSemitones", transposeSlider);
    transposeRandomAttachment = std::make_unique<ButtonAttachment> (state, "transposeRandom", transposeRandomButton);
    rotationAttachment = std::make_unique<SliderAttachment> (state, "rotationSteps", rotationSlider);
    rotationRandomAttachment = std::make_unique<ButtonAttachment> (state, "rotationRandom", rotationRandomButton);
    lengthAttachment = std::make_unique<SliderAttachment> (state, "lengthPercent", lengthSlider);
    lengthRandomAttachment = std::make_unique<ButtonAttachment> (state, "lengthRandom", lengthRandomButton);
    stretchAttachment = std::make_unique<SliderAttachment> (state, "stretchPercent", stretchSlider);
    stretchRandomAttachment = std::make_unique<ButtonAttachment> (state, "stretchRandom", stretchRandomButton);
    stretchQuantizedAttachment = std::make_unique<ButtonAttachment> (state, "stretchQuantized", stretchQuantizedButton);
    intervalAttachment = std::make_unique<SliderAttachment> (state, "intervalScalePercent", intervalSlider);
    intervalRandomAttachment = std::make_unique<ButtonAttachment> (state, "intervalRandom", intervalRandomButton);
    linkHubAttachment = std::make_unique<ButtonAttachment> (state, "linkHub", linkHubButton);
    broadcastChannelAttachment = std::make_unique<SliderAttachment> (state, "broadcastChannel", broadcastChannelSlider);
    listenChannelAttachment = std::make_unique<SliderAttachment> (state, "listenChannel", listenChannelSlider);
    outputChannelAttachment = std::make_unique<SliderAttachment> (state, "outputChannelOverride", outputChannelSlider);
    relayEnabledAttachment = std::make_unique<ButtonAttachment> (state, "relayEnabled", relayEnabledButton);
    instanceSeedAttachment = std::make_unique<SliderAttachment> (state, "instanceSeed", instanceSeedSlider);

    // Everything that belongs to the normal parameter view - see this
    // vector's own doc comment in the header. Deliberately listed once here
    // rather than a push_back scattered after each addAndMakeVisible above,
    // so this list is auditable in one place against what's actually shown.
    mainPanelComponents = {
        &bypassButton, &captureModeLabel, &captureModeBox,
        &holdBarsLabel, &holdBarsSlider, &holdBarsRandomButton, &holdBarsRangeSlider,
        &overlapModeLabel, &overlapModeBox,
        &phraseGapLabel, &phraseGapSlider,
        &minimumInterestLabel, &minimumInterestSlider,
        &callbackProbabilityLabel, &callbackProbabilitySlider,
        &autonomousFireLabel, &autonomousFireSlider,
        &ignoreKeyswitchesButton, &ksIgnoreMinLabel, &ksIgnoreMinSlider, &ksIgnoreMaxLabel, &ksIgnoreMaxSlider,
        &monophonicCaptureButton,
        &captureBankLabel, &captureBankBox, &clearBankButton,
        &activeBankLabel, &activeBankBox,
        &recencyBiasLabel, &recencyBiasSlider,
        &restlessnessLabel, &restlessnessSlider, &contentAwareWeightingButton,
        &transformLabel, &transformBox,
        &transposeLabel, &transposeSlider, &transposeRandomButton, &transposeRangeSlider,
        &rotationLabel, &rotationSlider, &rotationRandomButton, &rotationRangeSlider,
        &lengthLabel, &lengthSlider, &lengthRandomButton, &lengthRangeSlider,
        &stretchLabel, &stretchSlider, &stretchRandomButton, &stretchQuantizedButton, &stretchRangeSlider,
        &intervalLabel, &intervalSlider, &intervalRandomButton, &intervalRangeSlider,
        &linkHubLabel, &linkHubButton, &clearRemoteButton,
        &broadcastChannelLabel, &broadcastChannelSlider,
        &listenChannelLabel, &listenChannelSlider,
        &outputChannelLabel, &outputChannelSlider, &relayEnabledButton,
        &instanceSeedLabel, &instanceSeedSlider, &randomizeSeedButton,
        &instanceLabelLabel, &instanceLabelEditor,
        &presetSectionLabel, &savePresetButton, &loadPresetButton, &sendPresetButton, &presetStatusLabel,
    };

    // SS44: move every child added above INTO mainPanelContent, except the
    // handful that stay in the fixed header (title/subtitle/build/bypass/
    // matrixTabButton) or belong to the OTHER Viewport entirely
    // (matrixViewport) - see mainPanelContent's own doc comment in the
    // header for why this reparents-after-the-fact instead of retargeting
    // each of the 60+ addAndMakeVisible/addChildComponent calls above.
    // Component::addAndMakeVisible on a new parent automatically detaches a
    // child from whatever parent it already has, so this is a genuine move,
    // not a duplicate. Snapshot the list first - reparenting while iterating
    // getChildComponent(i) directly would shift indices out from under the
    // loop as each one is removed.
    {
        const std::array<juce::Component*, 6> keepOnEditor {
            &titleLabel, &subtitleLabel, &buildLabel,
            &bypassButton, &matrixTabButton, &matrixViewport
        };

        std::vector<juce::Component*> toMove;
        for (int i = 0; i < getNumChildComponents(); ++i)
        {
            auto* child = getChildComponent (i);
            if (std::find (keepOnEditor.begin(), keepOnEditor.end(), child) == keepOnEditor.end()
                && child != &mainPanelViewport)
                toMove.push_back (child);
        }

        // addChildComponent (not addAndMakeVisible) - preserves whatever
        // visible/hidden state each control already has from its own setup
        // above (several start hidden on purpose - e.g. the Hub-preset
        // buttons, gated on isHub in timerCallback) rather than forcing
        // every single one visible and relying on the very next
        // timerCallback() call below to quietly correct it.
        for (auto* child : toMove)
            mainPanelContent.addChildComponent (child);
    }

    startTimerHz (4);
    timerCallback();
}

OrchDelayAudioProcessorEditor::~OrchDelayAudioProcessorEditor()
{
    stopTimer();
}

void OrchDelayAudioProcessorEditor::paint (juce::Graphics& g)
{
    g.fillAll (kBackground);
}

void OrchDelayAudioProcessorEditor::resized()
{
    auto area = getLocalBounds().reduced (16);

    // Header spans the full width - shared by both columns below.
    titleLabel.setBounds (area.removeFromTop (40));
    subtitleLabel.setBounds (area.removeFromTop (22));
    buildLabel.setBounds (area.removeFromTop (18));
    area.removeFromTop (12);

    // matrixTabButton lives on this same row, ABOVE where matrixView's own
    // bounds start below - deliberately, not just for spacing. It used to
    // sit down near Listen Channel, inside the same area matrixView covers
    // when shown; matrixView is added as a child AFTER matrixTabButton (so
    // it draws on top) and fully occluded/ate its clicks, leaving no way
    // back to the main view once the matrix was open. Placing it here, in
    // the part of the layout captured BEFORE fullWidthArea/matrixView's own
    // bounds exist, makes that structurally impossible to regress into.
    auto bypassRow = area.removeFromTop (28);
    matrixTabButton.setBounds (bypassRow.removeFromRight (160));
    bypassRow.removeFromRight (8);
    bypassButton.setBounds (bypassRow);
    area.removeFromTop (12);

    // Status is placed AFTER the columns below (see the bottom of this
    // function), spanning this same full width - captured here, before the
    // column split, rather than pinned via removeFromBottom against the
    // window's own DECLARED height. A host (Bitwig's own device panel, in
    // particular) can silently render less vertical space than that
    // declared height with no scrollbar, which left a bottom-pinned status
    // label mostly cut off even though the actual column content ended
    // well above the visible edge - see this bug's own Docs SS27 addendum.
    const auto fullWidthArea = area;

    // SS44: everything below now lives inside mainPanelContent, a fixed-size
    // scrollable canvas (see its own doc comment in the header), rather than
    // directly in the outer editor's own bounds - mainPanelViewport (sized to
    // fullWidthArea at the bottom of this function, mirroring matrixViewport
    // just below it) scrolls it whenever the real window is shorter than the
    // content actually needs. kMainPanelContentHeight is deliberately
    // generous (comfortably taller than this content has ever measured) -
    // erring tall just leaves a little unused scroll space at the bottom,
    // never clips, and it only needs to be right once rather than
    // hand-recomputed on every future feature added here.
    static constexpr int kMainPanelContentWidth = 1000;
    static constexpr int kMainPanelContentHeight = 1360;   // +300 for Hub General Parameters (SS45)
    mainPanelContent.setSize (kMainPanelContentWidth, kMainPanelContentHeight);
    auto contentArea = mainPanelContent.getLocalBounds().reduced (16, 0);

    // Two columns for everything else - capture/timing on the left,
    // transform on the right. The single-column layout grew too tall
    // across this session's feature growth to fit a typical plugin window;
    // split here per direct user request.
    auto rightArea = contentArea.removeFromRight ((contentArea.getWidth() - 24) / 2);
    contentArea.removeFromRight (24);
    auto& leftArea = contentArea;

    auto leftRow = [&leftArea] (int height = 28) { return leftArea.removeFromTop (height); };
    auto rightRow = [&rightArea] (int height = 28) { return rightArea.removeFromTop (height); };

    // --- Left column: capture & timing ---------------------------------
    captureModeLabel.setBounds (leftRow (18));
    captureModeBox.setBounds (leftRow());
    leftArea.removeFromTop (5);

    holdBarsLabel.setBounds (leftRow (18));
    auto holdBarsRow = leftRow();
    holdBarsRandomButton.setBounds (holdBarsRow.removeFromRight (75));
    holdBarsRow.removeFromRight (8);
    holdBarsSlider.setBounds (holdBarsRow);
    holdBarsRangeSlider.setBounds (holdBarsRow);
    leftArea.removeFromTop (5);

    overlapModeLabel.setBounds (leftRow (18));
    overlapModeBox.setBounds (leftRow());
    leftArea.removeFromTop (5);

    phraseGapLabel.setBounds (leftRow (18));
    phraseGapSlider.setBounds (leftRow());
    leftArea.removeFromTop (5);

    minimumInterestLabel.setBounds (leftRow (18));
    minimumInterestSlider.setBounds (leftRow());
    leftArea.removeFromTop (5);

    callbackProbabilityLabel.setBounds (leftRow (18));
    callbackProbabilitySlider.setBounds (leftRow());
    leftArea.removeFromTop (5);

    autonomousFireLabel.setBounds (leftRow (18));
    autonomousFireSlider.setBounds (leftRow());
    leftArea.removeFromTop (5);

    ignoreKeyswitchesButton.setBounds (leftRow());
    leftArea.removeFromTop (5);
    ksIgnoreMinLabel.setBounds (leftRow (18));
    ksIgnoreMinSlider.setBounds (leftRow());
    leftArea.removeFromTop (5);
    ksIgnoreMaxLabel.setBounds (leftRow (18));
    ksIgnoreMaxSlider.setBounds (leftRow());
    leftArea.removeFromTop (5);

    monophonicCaptureButton.setBounds (leftRow());
    leftArea.removeFromTop (5);

    captureBankLabel.setBounds (leftRow (18));
    auto captureBankRow = leftRow();
    clearBankButton.setBounds (captureBankRow.removeFromRight (90));
    captureBankRow.removeFromRight (8);
    captureBankBox.setBounds (captureBankRow);
    leftArea.removeFromTop (5);

    activeBankLabel.setBounds (leftRow (18));
    activeBankBox.setBounds (leftRow());
    leftArea.removeFromTop (5);

    recencyBiasLabel.setBounds (leftRow (18));
    recencyBiasSlider.setBounds (leftRow());
    leftArea.removeFromTop (5);

    instanceSeedLabel.setBounds (leftRow (18));
    auto seedRow = leftRow();
    randomizeSeedButton.setBounds (seedRow.removeFromRight (90));
    seedRow.removeFromRight (8);
    instanceSeedSlider.setBounds (seedRow);
    leftArea.removeFromTop (5);

    instanceLabelLabel.setBounds (leftRow (18));
    instanceLabelEditor.setBounds (leftRow());
    leftArea.removeFromTop (5);

    // --- Right column: transform -----------------------------------------
    restlessnessLabel.setBounds (rightRow (18));
    auto restlessnessRow = rightRow();
    contentAwareWeightingButton.setBounds (restlessnessRow.removeFromRight (110));
    restlessnessRow.removeFromRight (8);
    restlessnessSlider.setBounds (restlessnessRow);
    rightArea.removeFromTop (5);

    transformLabel.setBounds (rightRow (18));
    transformBox.setBounds (rightRow());
    rightArea.removeFromTop (5);

    transposeLabel.setBounds (rightRow (18));
    auto transposeRow = rightRow();
    transposeRandomButton.setBounds (transposeRow.removeFromRight (90));
    transposeRow.removeFromRight (8);
    transposeSlider.setBounds (transposeRow);
    transposeRangeSlider.setBounds (transposeRow);
    rightArea.removeFromTop (5);

    rotationLabel.setBounds (rightRow (18));
    auto rotationRow = rightRow();
    rotationRandomButton.setBounds (rotationRow.removeFromRight (90));
    rotationRow.removeFromRight (8);
    rotationSlider.setBounds (rotationRow);
    rotationRangeSlider.setBounds (rotationRow);
    rightArea.removeFromTop (5);

    lengthLabel.setBounds (rightRow (18));
    auto lengthRow = rightRow();
    lengthRandomButton.setBounds (lengthRow.removeFromRight (90));
    lengthRow.removeFromRight (8);
    lengthSlider.setBounds (lengthRow);
    lengthRangeSlider.setBounds (lengthRow);
    rightArea.removeFromTop (5);

    stretchLabel.setBounds (rightRow (18));
    auto stretchRow = rightRow();
    stretchQuantizedButton.setBounds (stretchRow.removeFromRight (85));
    stretchRow.removeFromRight (6);
    stretchRandomButton.setBounds (stretchRow.removeFromRight (75));
    stretchRow.removeFromRight (8);
    stretchSlider.setBounds (stretchRow);
    stretchRangeSlider.setBounds (stretchRow);
    rightArea.removeFromTop (5);

    intervalLabel.setBounds (rightRow (18));
    auto intervalRow = rightRow();
    intervalRandomButton.setBounds (intervalRow.removeFromRight (75));
    intervalRow.removeFromRight (8);
    intervalSlider.setBounds (intervalRow);
    intervalRangeSlider.setBounds (intervalRow);
    rightArea.removeFromTop (5);

    // --- Right column, continued: cross-instance broadcast (Docs SS27) ---
    linkHubLabel.setBounds (rightRow (18));
    auto linkHubRow = rightRow();
    clearRemoteButton.setBounds (linkHubRow.removeFromRight (100));
    linkHubRow.removeFromRight (8);
    linkHubButton.setBounds (linkHubRow);
    rightArea.removeFromTop (5);

    broadcastChannelLabel.setBounds (rightRow (18));
    broadcastChannelSlider.setBounds (rightRow());
    rightArea.removeFromTop (5);

    listenChannelLabel.setBounds (rightRow (18));
    listenChannelSlider.setBounds (rightRow());
    rightArea.removeFromTop (5);

    outputChannelLabel.setBounds (rightRow (18));
    outputChannelSlider.setBounds (rightRow());
    rightArea.removeFromTop (5);

    relayEnabledButton.setBounds (rightRow());
    rightArea.removeFromTop (5);

    // --- Right column, continued: Hub-pushed presets (SS39) --------------
    // Visibility is isHub-gated (see timerCallback) same as the Matrix tab,
    // but bounds are always set regardless - a hidden component's bounds
    // simply never get painted, and setting them unconditionally means this
    // block never has to duplicate isHub's own logic.
    rightArea.removeFromTop (6);
    presetSectionLabel.setBounds (rightRow (18));
    auto presetButtonsRow = rightRow (26);
    const int presetButtonWidth = presetButtonsRow.getWidth() / 3 - 6;
    savePresetButton.setBounds (presetButtonsRow.removeFromLeft (presetButtonWidth));
    presetButtonsRow.removeFromLeft (9);
    loadPresetButton.setBounds (presetButtonsRow.removeFromLeft (presetButtonWidth));
    presetButtonsRow.removeFromLeft (9);
    sendPresetButton.setBounds (presetButtonsRow);
    rightArea.removeFromTop (4);
    presetStatusLabel.setBounds (rightRow (16));
    rightArea.removeFromTop (5);

    // Hub-pushed full reset (SS41) - own row below the preset section rather
    // than folded into presetButtonsRow above, per resetAllButton's own doc
    // comment (wipes musical content, not routing - kept visually distinct
    // from the three routing-preset actions above it). Same
    // always-set-bounds-regardless-of-visibility reasoning as the preset row.
    resetAllButton.setBounds (rightRow (26));
    rightArea.removeFromTop (5);

    // SS45: Hub "General Parameters" - own section below Reset All Instances,
    // same always-set-bounds-regardless-of-visibility reasoning as the preset
    // row above.
    rightArea.removeFromTop (6);
    shapeSectionLabel.setBounds (rightRow (18));
    rightArea.removeFromTop (2);

    auto placeShapeSliderRow = [&] (juce::Label& label, juce::Slider& slider)
    {
        label.setBounds (rightRow (16));
        slider.setBounds (rightRow (22));
        rightArea.removeFromTop (4);
    };

    placeShapeSliderRow (shapeHoldBarsLabel, shapeHoldBarsSlider);
    placeShapeSliderRow (shapeAutonomousFireLabel, shapeAutonomousFireSlider);
    placeShapeSliderRow (shapePhraseGapLabel, shapePhraseGapSlider);
    placeShapeSliderRow (shapeMinimumInterestLabel, shapeMinimumInterestSlider);
    placeShapeSliderRow (shapeCallbackProbabilityLabel, shapeCallbackProbabilitySlider);

    auto shapeToggleRow = rightRow (24);
    shapeMonophonicCaptureButton.setBounds (shapeToggleRow.removeFromLeft (shapeToggleRow.getWidth() / 2));
    shapeIgnoreKeyswitchesButton.setBounds (shapeToggleRow);
    rightArea.removeFromTop (4);

    auto shapeButtonRow = rightRow (26);
    sendShapeButton.setBounds (shapeButtonRow.removeFromLeft ((shapeButtonRow.getWidth() - 8) / 2));
    shapeButtonRow.removeFromLeft (8);
    randomizeSendShapeButton.setBounds (shapeButtonRow);
    rightArea.removeFromTop (5);

    // Status goes directly below whichever column ended up taller (today,
    // the left one) - in mainPanelContent's own local coordinate space now
    // (SS44), same reasoning as ever for not pinning to a declared bottom
    // edge, see this function's own comment above `fullWidthArea` for why.
    const int contentBottom = juce::jmax (leftArea.getY(), rightArea.getY());
    juce::Rectangle<int> statusArea (0, contentBottom + 6, kMainPanelContentWidth, 68);
    statusLabel.setBounds (statusArea);

    // SS44: mainPanelViewport fills the same fullWidthArea matrixViewport
    // does just below - only one of the two is ever visible at a time (see
    // setShowingMatrix), so they can share the real estate exactly like
    // matrixViewport already shares it with the (pre-SS44) main view.
    mainPanelViewport.setBounds (fullWidthArea);

    // Connection Matrix (Docs SS31) fills the exact same real estate the main
    // panel viewport above occupies, so toggling it never resizes the window.
    // The viewport (not matrixView itself) gets this bounds - matrixView
    // sizes itself to its own full content in setRows() and scrolls within
    // whatever the viewport shows once that's larger (2026-09-26: a real rig
    // runs ~44 instances, far more than fit in one unscrolled window).
    matrixViewport.setBounds (fullWidthArea);
}

void OrchDelayAudioProcessorEditor::setupRandomRangeBinding (juce::Slider& rangeSlider, juce::Slider& manualSlider,
                                                              juce::ToggleButton& randomButton, juce::Label& label,
                                                              bool isPercent,
                                                              const juce::String& minParamId, const juce::String& maxParamId)
{
    auto& state = audioProcessor.getParameters();
    auto* minParam = dynamic_cast<juce::RangedAudioParameter*> (state.getParameter (minParamId));
    auto* maxParam = dynamic_cast<juce::RangedAudioParameter*> (state.getParameter (maxParamId));
    auto* minParamRaw = state.getRawParameterValue (minParamId);
    auto* maxParamRaw = state.getRawParameterValue (maxParamId);
    jassert (minParam != nullptr && maxParam != nullptr);

    rangeSlider.setSliderStyle (juce::Slider::TwoValueHorizontal);
    rangeSlider.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
    rangeSlider.setColour (juce::Slider::thumbColourId, kThumb);
    rangeSlider.setColour (juce::Slider::trackColourId, kTrack);
    if (minParam != nullptr)
    {
        const auto& range = minParam->getNormalisableRange();
        rangeSlider.setRange (range.start, range.end, range.interval);
    }
    if (minParamRaw != nullptr && maxParamRaw != nullptr)
        rangeSlider.setMinAndMaxValues (minParamRaw->load(), maxParamRaw->load(), juce::dontSendNotification);
    addChildComponent (rangeSlider);   // starts hidden - shown only when Random is on, see timerCallback

    // Hand-wired instead of a SliderAttachment (which only supports
    // single-value sliders) - see this member's own doc comment in the
    // header for why. Gesture-bracketed so host automation/undo records it
    // as one drag, not a value snapping in with no gesture at all.
    rangeSlider.onDragStart = [minParam, maxParam]
    {
        if (minParam != nullptr) minParam->beginChangeGesture();
        if (maxParam != nullptr) maxParam->beginChangeGesture();
    };
    rangeSlider.onDragEnd = [minParam, maxParam]
    {
        if (minParam != nullptr) minParam->endChangeGesture();
        if (maxParam != nullptr) maxParam->endChangeGesture();
    };
    rangeSlider.onValueChange = [&rangeSlider, minParam, maxParam]
    {
        if (minParam != nullptr)
            minParam->setValueNotifyingHost (minParam->convertTo0to1 (static_cast<float> (rangeSlider.getMinValue())));
        if (maxParam != nullptr)
            maxParam->setValueNotifyingHost (maxParam->convertTo0to1 (static_cast<float> (rangeSlider.getMaxValue())));
    };

    RandomRangeBinding binding;
    binding.rangeSlider = &rangeSlider;
    binding.manualSlider = &manualSlider;
    binding.randomButton = &randomButton;
    binding.label = &label;
    binding.baseLabelText = label.getText();
    binding.isPercent = isPercent;
    binding.minParamRaw = minParamRaw;
    binding.maxParamRaw = maxParamRaw;
    binding.minParam = minParam;
    binding.maxParam = maxParam;
    randomRangeBindings.push_back (binding);
}

void OrchDelayAudioProcessorEditor::setShowingMatrix (bool shouldShow)
{
    showingMatrix = shouldShow;
    matrixTabButton.setToggleState (shouldShow, juce::dontSendNotification);
    matrixTabButton.setButtonText (shouldShow ? "< Back to Main" : "Connection Matrix");

    for (auto* c : mainPanelComponents)
        c->setVisible (! shouldShow);

    matrixViewport.setVisible (shouldShow);

    // Unconditional both ways: showing the matrix needs fresh data painted
    // immediately rather than waiting for the next tick, and returning to
    // the main view needs the manual-vs-range slider visibility pass
    // (below) to run right away too - otherwise the blanket "show every
    // main-panel component" loop above briefly shows BOTH a parameter's
    // manual slider and its range slider at once, until the next tick.
    timerCallback();
}

// Hub-pushed presets (SS39) - see this file's own header comment on
// pendingPresetEntries for the overall Save/Load/Send split.
void OrchDelayAudioProcessorEditor::capturePresetToBuffer()
{
    pendingPresetEntries = audioProcessor.getLink().capturePresetForUi();
    presetStatusLabel.setText ("Captured " + juce::String (pendingPresetEntries.size()) + " instance(s) - not sent yet.",
                                juce::dontSendNotification);
}

void OrchDelayAudioProcessorEditor::savePresetToFile (const juce::File& file)
{
    juce::Array<juce::var> entriesVar;
    for (const auto& entry : pendingPresetEntries)
    {
        auto* obj = new juce::DynamicObject();
        obj->setProperty ("label", entry.label);
        obj->setProperty ("broadcastChannel", entry.broadcastChannel);
        obj->setProperty ("listenChannel", entry.listenChannel);
        obj->setProperty ("relayEnabled", entry.relayEnabled);
        obj->setProperty ("activeBank", entry.activeBank);
        obj->setProperty ("outputChannelOverride", entry.outputChannelOverride);
        entriesVar.add (juce::var (obj));
    }

    auto* root = new juce::DynamicObject();
    root->setProperty ("orchDelayPresetVersion", 1);
    root->setProperty ("entries", entriesVar);

    if (file.replaceWithText (juce::JSON::toString (juce::var (root), true)))
        presetStatusLabel.setText ("Saved " + juce::String (pendingPresetEntries.size()) + " instance(s) to "
                                        + file.getFileName(), juce::dontSendNotification);
    else
        presetStatusLabel.setText ("Failed to write " + file.getFullPathName(), juce::dontSendNotification);
}

void OrchDelayAudioProcessorEditor::loadPresetFromFile (const juce::File& file)
{
    juce::var parsed;
    if (! juce::JSON::parse (file.loadFileAsString(), parsed).wasOk() || ! parsed.isObject())
    {
        presetStatusLabel.setText ("Failed to read " + file.getFullPathName(), juce::dontSendNotification);
        return;
    }

    std::vector<OrchDelayLink::PresetEntry> loaded;
    if (auto* entries = parsed.getProperty ("entries", juce::var()).getArray())
    {
        for (const auto& entryVar : *entries)
        {
            OrchDelayLink::PresetEntry entry;
            entry.label = entryVar.getProperty ("label", "").toString();
            entry.broadcastChannel = static_cast<int> (entryVar.getProperty ("broadcastChannel", 0));
            entry.listenChannel = static_cast<int> (entryVar.getProperty ("listenChannel", 0));
            entry.relayEnabled = static_cast<bool> (entryVar.getProperty ("relayEnabled", false));
            entry.activeBank = static_cast<int> (entryVar.getProperty ("activeBank", 0));
            entry.outputChannelOverride = static_cast<int> (entryVar.getProperty ("outputChannelOverride", 0));
            loaded.push_back (entry);
        }
    }

    pendingPresetEntries = std::move (loaded);
    presetStatusLabel.setText ("Loaded " + juce::String (pendingPresetEntries.size()) + " instance(s) from "
                                    + file.getFileName() + " - not sent yet.", juce::dontSendNotification);
}

void OrchDelayAudioProcessorEditor::sendPendingPreset()
{
    if (pendingPresetEntries.empty())
    {
        presetStatusLabel.setText ("Nothing to send - Save or Load a preset first.", juce::dontSendNotification);
        return;
    }

    audioProcessor.getLink().sendPreset (pendingPresetEntries);
    presetStatusLabel.setText ("Sent preset to " + juce::String (pendingPresetEntries.size()) + " instance(s).",
                                juce::dontSendNotification);
}

OrchDelayLink::ShapeEntry OrchDelayAudioProcessorEditor::buildShapeFromUi() const
{
    OrchDelayLink::ShapeEntry shape;
    shape.holdBars = static_cast<int> (shapeHoldBarsSlider.getValue());
    shape.autonomousFireBars = static_cast<int> (shapeAutonomousFireSlider.getValue());
    shape.phraseGapBeats = static_cast<float> (shapePhraseGapSlider.getValue());
    shape.minimumInterest = static_cast<float> (shapeMinimumInterestSlider.getValue());
    shape.callbackProbability = static_cast<float> (shapeCallbackProbabilitySlider.getValue());
    shape.monophonicCapture = shapeMonophonicCaptureButton.getToggleState();
    shape.ignoreKeyswitches = shapeIgnoreKeyswitchesButton.getToggleState();
    return shape;
}

void OrchDelayAudioProcessorEditor::ConnectionMatrixView::setRows (std::vector<Row> newRows)
{
    rows = std::move (newRows);

    // Size to natural content (fixed-per-cell x row count), not whatever the
    // owning Viewport happens to be showing - see computeGridGeometry()'s own
    // comment. setSize() is a no-op if unchanged, so this is safe to call on
    // every refresh tick.
    const auto geo = computeGridGeometry();
    setSize (geo.leftGutter + geo.n * geo.cellW, geo.topStrip + geo.n * geo.cellH);

    repaint();
}

OrchDelayAudioProcessorEditor::ConnectionMatrixView::GridGeometry
    OrchDelayAudioProcessorEditor::ConnectionMatrixView::computeGridGeometry() const
{
    const int n = static_cast<int> (rows.size());
    const int leftGutter = 200;
    const int topStrip = 40;   // taller than a bare index needs - now holds a real (short) name

    // Fixed per-cell size regardless of how many rows there are - a real rig
    // runs ~44 instances (2026-09-25/26 live use), far more than fit legibly
    // in any one window. Previously this divided the visible area by n,
    // silently shrinking (and eventually clipping) every cell as the rig
    // grew; now the grid always renders at a legible fixed size, growing
    // past the viewport's own visible bounds instead - setRows() sizes this
    // component to match, and the owning editor's Viewport (not this
    // component) provides scrolling once that exceeds what's on screen.
    const int cellW = 90;
    const int cellH = 26;
    return { leftGutter, topStrip, cellW, cellH, leftGutter, topStrip, n };
}

void OrchDelayAudioProcessorEditor::ConnectionMatrixView::mouseDown (const juce::MouseEvent& e)
{
    if (rows.empty())
        return;

    const auto geo = computeGridGeometry();
    const int localX = e.x - geo.gridX;
    const int localY = e.y - geo.gridY;
    if (localX < 0 || localY < 0 || localX >= geo.n * geo.cellW || localY >= geo.n * geo.cellH)
        return;   // click landed outside the grid itself (label gutter, legend, etc.)

    const int destColumn = localX / geo.cellW;   // column = destination (Listen Channel)
    const int sourceRow = localY / geo.cellH;    // row = source (Broadcast Channel)

    if (sourceRow == destColumn)
        return;   // diagonal - an instance "connecting to itself" means nothing
    if (rows[static_cast<size_t> (sourceRow)].broadcastChannel <= 0)
        return;   // source isn't broadcasting on anything - nothing a click could wire up

    if (onCellClicked != nullptr)
        onCellClicked (sourceRow, destColumn);
}

void OrchDelayAudioProcessorEditor::ConnectionMatrixView::paint (juce::Graphics& g)
{
    g.fillAll (kBackground);

    if (rows.empty())
    {
        g.setColour (kMuted);
        g.setFont (juce::FontOptions (14.0f));
        g.drawFittedText ("No connections yet - waiting for other instances to connect.",
                          getLocalBounds().reduced (16), juce::Justification::centred, 2);
        return;
    }

    const auto geo = computeGridGeometry();
    const int n = geo.n;
    const int cellW = geo.cellW;
    const int cellH = geo.cellH;
    const int gridX = geo.gridX;
    const int gridY = geo.gridY;
    const int leftGutter = geo.leftGutter;
    const int topStrip = geo.topStrip;

    auto displayNameFor = [] (const Row& row)
    {
        auto display = row.label.isNotEmpty() ? row.label
                                              : (row.isSelf ? juce::String ("(this instance)")
                                                            : juce::String ("(unlabeled)"));
        if (row.isSelf)
            display += " *";
        return display;
    };

    // Row labels (sources).
    g.setFont (juce::FontOptions (12.0f));
    for (int i = 0; i < n; ++i)
    {
        const auto& row = rows[i];
        g.setColour (row.isSelf ? juce::Colours::white : kMuted);
        juce::Rectangle<int> labelArea (0, gridY + i * cellH, leftGutter - 8, cellH);
        g.drawFittedText (displayNameFor (row), labelArea, juce::Justification::centredRight, 1);

        g.setColour (kMuted.withAlpha (0.7f));
        juce::Rectangle<int> chanArea (gridX + n * cellW + 6, gridY + i * cellH, 60, cellH);
        g.drawFittedText ("bc" + juce::String (row.broadcastChannel) + " lc" + juce::String (row.listenChannel),
                          chanArea, juce::Justification::centredLeft, 1);
    }

    // Column headers (destinations) - the SAME name as the matching row,
    // not a bare index: a number forced cross-referencing back to a row
    // label to find out what it meant, which is exactly what a live user
    // reported as the actual source of confusion, not the grid concept
    // itself. Small font + auto-shrink-to-fit (drawFittedText) rather than
    // manual truncation - simple prefix-truncation collides badly on
    // similarly-named instances (e.g. an orchestra's own "Violin 1"/
    // "Violin 2").
    g.setFont (juce::FontOptions (11.0f));
    for (int j = 0; j < n; ++j)
    {
        g.setColour (rows[j].isSelf ? juce::Colours::white : kMuted);
        juce::Rectangle<int> headerArea (gridX + j * cellW + 2, 2, cellW - 4, topStrip - 4);
        g.drawFittedText (displayNameFor (rows[j]), headerArea, juce::Justification::centred, 2);
    }

    // Grid lines.
    g.setColour (kOutline.withAlpha (0.4f));
    for (int row = 0; row <= n; ++row)
        g.drawLine (static_cast<float> (gridX), static_cast<float> (gridY + row * cellH),
                   static_cast<float> (gridX + n * cellW), static_cast<float> (gridY + row * cellH));
    for (int col = 0; col <= n; ++col)
        g.drawLine (static_cast<float> (gridX + col * cellW), static_cast<float> (gridY),
                   static_cast<float> (gridX + col * cellW), static_cast<float> (gridY + n * cellH));

    // Cells: (i, j) lit when row i's Broadcast Channel feeds row j's Listen
    // Channel - i.e. row i sends TO column j. A column fed by more than one
    // distinct source is a real configuration hazard (two generators
    // reusing the same channel number, both landing on one follower) -
    // flagged in a warning colour rather than silently drawn the same as a
    // clean single-source connection.
    for (int j = 0; j < n; ++j)
    {
        int sourceCount = 0;
        for (int i = 0; i < n; ++i)
            if (i != j && rows[i].broadcastChannel > 0 && rows[i].broadcastChannel == rows[j].listenChannel)
                ++sourceCount;

        for (int i = 0; i < n; ++i)
        {
            juce::Rectangle<int> cellArea (gridX + j * cellW + 2, gridY + i * cellH + 2, cellW - 4, cellH - 4);

            if (i == j)
            {
                g.setColour (kOutline.withAlpha (0.15f));
                g.fillRect (cellArea);
                continue;
            }

            const bool lit = rows[i].broadcastChannel > 0 && rows[i].broadcastChannel == rows[j].listenChannel;
            if (! lit)
                continue;

            g.setColour (sourceCount > 1 ? juce::Colour::fromRGB (235, 140, 60) : kThumb);
            g.fillRoundedRectangle (cellArea.toFloat(), 3.0f);
        }
    }

    g.setColour (kMuted.withAlpha (0.7f));
    g.setFont (juce::FontOptions (11.0f));
    g.drawText ("Click a cell to connect that row's Broadcast Channel to that column's Listen Channel "
               "\xc2\xb7 click a lit cell to disconnect  \xc2\xb7 orange = two sources sharing one channel",
               getLocalBounds().removeFromBottom (16), juce::Justification::centred);
}

juce::String OrchDelayAudioProcessorEditor::getLinkStatusText() const
{
    auto& link = audioProcessor.getLink();

    switch (link.getModeForUi())
    {
        case OrchDelayLink::Mode::Hub:
            return "hub";
        case OrchDelayLink::Mode::HubPortBusy:
            return "hub(busy)";
        case OrchDelayLink::Mode::Client:
        default:
            return link.isConnectedForUi() ? "client(ok)" : "client(--)";
    }
}

void OrchDelayAudioProcessorEditor::timerCallback()
{
    // Connection Matrix (Docs SS31) only ever has real data on the hub
    // itself - hide the tab (and force back to the normal view if Hub gets
    // unchecked while the matrix happens to be showing) whenever this
    // instance isn't currently the hub.
    const bool isHub = linkHubButton.getToggleState();
    matrixTabButton.setVisible (isHub);
    if (! isHub && showingMatrix)
        setShowingMatrix (false);

    // Hub-pushed presets (SS39) - same isHub gate as the Matrix tab above;
    // hidden (not just disabled) when this instance isn't the hub, same
    // reasoning as matrixTabButton's own doc comment - a non-hub instance
    // has no live table of other instances to capture a preset from at all.
    presetSectionLabel.setVisible (isHub);
    savePresetButton.setVisible (isHub);
    loadPresetButton.setVisible (isHub);
    sendPresetButton.setVisible (isHub);
    presetStatusLabel.setVisible (isHub);

    // Hub-pushed full reset (SS41) - same isHub gate as the preset section
    // above, same reasoning: only the hub has any live connections to fan a
    // reset out to.
    resetAllButton.setVisible (isHub);

    // SS45: Hub "General Parameters" - same isHub gate as the sections
    // above, same reasoning: this only means anything once there's a live
    // rig of connected clients to push a template out to.
    shapeSectionLabel.setVisible (isHub);
    shapeHoldBarsLabel.setVisible (isHub);
    shapeHoldBarsSlider.setVisible (isHub);
    shapeAutonomousFireLabel.setVisible (isHub);
    shapeAutonomousFireSlider.setVisible (isHub);
    shapePhraseGapLabel.setVisible (isHub);
    shapePhraseGapSlider.setVisible (isHub);
    shapeMinimumInterestLabel.setVisible (isHub);
    shapeMinimumInterestSlider.setVisible (isHub);
    shapeCallbackProbabilityLabel.setVisible (isHub);
    shapeCallbackProbabilitySlider.setVisible (isHub);
    shapeMonophonicCaptureButton.setVisible (isHub);
    shapeIgnoreKeyswitchesButton.setVisible (isHub);
    sendShapeButton.setVisible (isHub);
    randomizeSendShapeButton.setVisible (isHub);

    // Instance Label root cause (SS37, confirmed via temp logging): Bitwig
    // constructs this editor BEFORE calling setStateInformation on the
    // processor, so the setText() in the constructor always reads an empty
    // label - the real saved value lands in the processor a moment later
    // with nothing to push it into the already-built TextEditor. The saved
    // state itself was never the problem. Poll it here instead, skipping
    // while the field has focus so an in-progress edit is never clobbered,
    // and skipping the write when the text already matches so a normal
    // (already-synced) tick never disturbs cursor position/selection.
    if (! instanceLabelEditor.hasKeyboardFocus (false))
    {
        const auto currentLabel = audioProcessor.getInstanceLabelForUi();
        if (instanceLabelEditor.getText() != currentLabel)
            instanceLabelEditor.setText (currentLabel, juce::dontSendNotification);
    }

    if (showingMatrix)
    {
        std::vector<ConnectionMatrixView::Row> rows;

        ConnectionMatrixView::Row self;
        self.label = audioProcessor.getInstanceLabelForUi();
        self.broadcastChannel = static_cast<int> (broadcastChannelSlider.getValue());
        self.listenChannel = static_cast<int> (listenChannelSlider.getValue());
        self.isSelf = true;
        rows.push_back (self);

        for (const auto& status : audioProcessor.getLink().getRemoteStatusesForUi())
        {
            ConnectionMatrixView::Row row;
            row.label = status.label;
            row.broadcastChannel = status.broadcastChannel;
            row.listenChannel = status.listenChannel;
            row.connectionId = status.connectionId;
            rows.push_back (row);
        }

        // Row/column order was previously whatever std::map<HubConnection*,...>
        // happened to iterate in - the raw pointer value of an internal
        // connection object, meaningless and liable to change between
        // sessions. Sorted here instead by Broadcast Channel (ascending,
        // non-broadcasting instances - channel 0 - pushed to the end since
        // they can never be a source anyway), tie-broken by label. Neither
        // is the same as the DAW's own track order (this plugin has no way
        // to know that), but both ARE something the user sets deliberately
        // and can rely on staying put - a real, predictable position to
        // build a mental map from, instead of an implementation accident.
        std::stable_sort (rows.begin(), rows.end(), [] (const ConnectionMatrixView::Row& a, const ConnectionMatrixView::Row& b)
        {
            const int aKey = a.broadcastChannel > 0 ? a.broadcastChannel : 1000;
            const int bKey = b.broadcastChannel > 0 ? b.broadcastChannel : 1000;
            if (aKey != bKey)
                return aKey < bKey;
            return a.label.compareIgnoreCase (b.label) < 0;
        });

        matrixView.setRows (std::move (rows));
        return;   // nothing else on screen to refresh while the matrix is up
    }

    // Random-range two-thumb sliders (Docs SS32): swap each manual slider
    // for its own range slider when that parameter's Random toggle is on,
    // and keep the range slider's displayed thumbs (and its label's live
    // "X to Y" text) in sync with the actual Min/Max parameter values -
    // unless the user is mid-drag, which would otherwise fight the drag.
    for (auto& binding : randomRangeBindings)
    {
        const bool randomOn = binding.randomButton != nullptr && binding.randomButton->getToggleState();
        binding.manualSlider->setVisible (! randomOn);
        binding.rangeSlider->setVisible (randomOn);

        if (! randomOn)
        {
            binding.label->setText (binding.baseLabelText, juce::dontSendNotification);
            continue;
        }

        const float lo = binding.minParamRaw != nullptr ? binding.minParamRaw->load() : 0.0f;
        const float hi = binding.maxParamRaw != nullptr ? binding.maxParamRaw->load() : 0.0f;

        if (! binding.rangeSlider->isMouseButtonDown())
            binding.rangeSlider->setMinAndMaxValues (lo, hi, juce::dontSendNotification);

        juce::String text = binding.baseLabelText + " - ";
        text += binding.isPercent ? (juce::String (lo, 1) + "% to " + juce::String (hi, 1) + "%")
                                  : (juce::String (juce::roundToInt (lo)) + " to " + juce::String (juce::roundToInt (hi)));
        if (binding.rangeSlider == &stretchRangeSlider && stretchQuantizedButton.getToggleState())
            text += " (quantized)";
        binding.label->setText (text, juce::dontSendNotification);
    }

    const bool haveTransport = audioProcessor.hasTransportForUi();
    const int pending = audioProcessor.pendingPhraseCountForUi();

    if (! haveTransport)
    {
        statusLabel.setText ("No transport - Standalone/no host clock: OrchDelay is inactive",
                             juce::dontSendNotification);
        return;
    }

    // Session-lifetime counters (reset only on prepareToPlay), not just the
    // live pending count - added so a silent session can be told apart at a
    // glance: MIDI never arrived (captured stays 0) vs. arrived but never
    // closed into a phrase (closed stays 0) vs. closed but never fired yet
    // (fired stays 0, still mid-hold).
    statusLabel.setText (juce::String (pending) + " pend | cap " +
                         juce::String (audioProcessor.notesCapturedForUi()) + " cls " +
                         juce::String (audioProcessor.phrasesClosedForUi()) + " fire " +
                         juce::String (audioProcessor.phrasesFiredForUi()) + " skip " +
                         juce::String (audioProcessor.skippedBusyForUi()) + " stop " +
                         juce::String (audioProcessor.stopEventsForUi()) + " rwSeen " +
                         juce::String (audioProcessor.rewindDetectedForUi()) + " rwAct " +
                         juce::String (audioProcessor.rewindActedForUi()) + "\nsched " +
                         juce::String (audioProcessor.lastScheduledFirePpqForUi(), 2) + "  reached " +
                         juce::String (audioProcessor.furthestBlockPpqForUi(), 2) + "\nchoice " +
                         juce::String (audioProcessor.lastManualChoiceForUi()) + " xform " +
                         juce::String (audioProcessor.lastChosenTransformForUi()) + " stretch% " +
                         juce::String (audioProcessor.lastResolvedStretchPercentForUi(), 1) + "\nskipQ " +
                         juce::String (audioProcessor.skippedQualityForUi()) + " interest " +
                         juce::String (audioProcessor.lastPhraseInterestForUi(), 1) + " callbacks " +
                         juce::String (audioProcessor.memoryCallbacksForUi()) + " autofire " +
                         juce::String (audioProcessor.autonomousFiresForUi()) + "\n" +
                         getLinkStatusText() + " remoteIn " +
                         juce::String (audioProcessor.remoteReceivedForUi()) + " relayed " +
                         juce::String (audioProcessor.relaysForUi()),
                         juce::dontSendNotification);
}
