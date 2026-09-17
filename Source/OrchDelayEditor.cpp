#include "OrchDelayEditor.h"
#include "OrchDelayBuildInfo.h"
#include "OrchDelayLink.h"

#include <algorithm>

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
    // Fixed at 1040x860 with no way to resize used to be fine, but this
    // session added a full row (matrix button), an Instance Label field,
    // and 6 range sliders without ever re-checking against a real screen -
    // a live user's own 1536x864 logical display leaves barely any margin
    // for the window's own titlebar and the OS taskbar once you add 860px
    // of content height, and with no resize capability at all there was no
    // way to work around it. Trimmed the default height a little and made
    // the window genuinely resizable (bounded, so it can't become unusably
    // small) as the durable fix - works for this screen and any other.
    // Tightened row spacing (8px -> 5px) and the status readout's own font/
    // height buy back real space, but a screen this size (1536x864 logical)
    // simply doesn't have room for the full content height AND comfortable
    // OS chrome margin at any reasonable default - resizability (below) is
    // the actual fix, not this specific number; 820 just starts closer to
    // right than the original 860 did.
    setResizable (true, true);
    setResizeLimits (900, 650, 1400, 1000);
    setSize (1040, 820);

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
    auto commitLabel = [this] { audioProcessor.setInstanceLabel (instanceLabelEditor.getText()); };
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

    addChildComponent (matrixView);   // starts hidden - shown via setShowingMatrix

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
        &instanceSeedLabel, &instanceSeedSlider, &randomizeSeedButton,
        &instanceLabelLabel, &instanceLabelEditor,
    };

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

    // Two columns for everything else - capture/timing on the left,
    // transform on the right. The single-column layout grew too tall
    // across this session's feature growth to fit a typical plugin window;
    // split here per direct user request.
    auto rightArea = area.removeFromRight ((area.getWidth() - 24) / 2);
    area.removeFromRight (24);
    auto& leftArea = area;

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

    // Status goes directly below whichever column ended up taller (today,
    // the left one) - never pinned to the window's own declared bottom edge,
    // see this function's own comment above `fullWidthArea` for why.
    const int contentBottom = juce::jmax (leftArea.getY(), rightArea.getY());
    juce::Rectangle<int> statusArea (fullWidthArea.getX(), contentBottom + 6,
                                     fullWidthArea.getWidth(), 68);
    statusLabel.setBounds (statusArea);

    // Connection Matrix (Docs SS31) fills the exact same real estate the two
    // columns above occupy, so toggling it never resizes the window.
    matrixView.setBounds (fullWidthArea.getX(), fullWidthArea.getY(),
                          fullWidthArea.getWidth(), contentBottom - fullWidthArea.getY());
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

    matrixView.setVisible (shouldShow);

    // Unconditional both ways: showing the matrix needs fresh data painted
    // immediately rather than waiting for the next tick, and returning to
    // the main view needs the manual-vs-range slider visibility pass
    // (below) to run right away too - otherwise the blanket "show every
    // main-panel component" loop above briefly shows BOTH a parameter's
    // manual slider and its range slider at once, until the next tick.
    timerCallback();
}

void OrchDelayAudioProcessorEditor::ConnectionMatrixView::setRows (std::vector<Row> newRows)
{
    rows = std::move (newRows);
    repaint();
}

OrchDelayAudioProcessorEditor::ConnectionMatrixView::GridGeometry
    OrchDelayAudioProcessorEditor::ConnectionMatrixView::computeGridGeometry() const
{
    const int n = static_cast<int> (rows.size());
    const int leftGutter = 200;
    const int topStrip = 40;   // taller than a bare index needs - now holds a real (short) name

    const auto gridArea = getLocalBounds().withTrimmedLeft (leftGutter).withTrimmedTop (topStrip);
    // Columns need real width to show a destination's own name (see this
    // struct's own doc comment) - independent of row height, which stays
    // compact since row labels already have the whole left gutter.
    const int cellW = juce::jlimit (70, 130, gridArea.getWidth() / juce::jmax (1, n));
    const int cellH = juce::jlimit (22, 48, gridArea.getHeight() / juce::jmax (1, n));
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
                         juce::String (audioProcessor.remoteReceivedForUi()),
                         juce::dontSendNotification);
}
