#include "OrchDelayEditor.h"
#include "OrchDelayBuildInfo.h"

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
    setSize (520, 914);

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

    // "Random" - when on, Transpose becomes a symmetric range bound instead
    // of a fixed amount (see odly::resolveRandomTransposeSemitones).
    transposeRandomButton.setButtonText ("Random");
    transposeRandomButton.setColour (juce::ToggleButton::textColourId, juce::Colours::white);
    addAndMakeVisible (transposeRandomButton);

    setupLabel (rotationLabel, "Rotation (steps)");
    addAndMakeVisible (rotationLabel);
    setupSlider (rotationSlider);
    rotationRandomButton.setButtonText ("Random");
    rotationRandomButton.setColour (juce::ToggleButton::textColourId, juce::Colours::white);
    addAndMakeVisible (rotationRandomButton);

    setupLabel (lengthLabel, "Length (%)");
    addAndMakeVisible (lengthLabel);
    setupSlider (lengthSlider);
    lengthRandomButton.setButtonText ("Random");
    lengthRandomButton.setColour (juce::ToggleButton::textColourId, juce::Colours::white);
    addAndMakeVisible (lengthRandomButton);

    setupLabel (stretchLabel, "Stretch (%)");
    addAndMakeVisible (stretchLabel);
    setupSlider (stretchSlider);
    stretchRandomButton.setButtonText ("Random");
    stretchRandomButton.setColour (juce::ToggleButton::textColourId, juce::Colours::white);
    addAndMakeVisible (stretchRandomButton);
    stretchQuantizedButton.setButtonText ("Quantize");
    stretchQuantizedButton.setColour (juce::ToggleButton::textColourId, juce::Colours::white);
    addAndMakeVisible (stretchQuantizedButton);

    setupLabel (intervalLabel, "Interval Scale (%)");
    addAndMakeVisible (intervalLabel);
    setupSlider (intervalSlider);
    intervalRandomButton.setButtonText ("Random");
    intervalRandomButton.setColour (juce::ToggleButton::textColourId, juce::Colours::white);
    addAndMakeVisible (intervalRandomButton);

    setupLabel (instanceSeedLabel, "Instance Seed");
    addAndMakeVisible (instanceSeedLabel);
    setupSlider (instanceSeedSlider);

    randomizeSeedButton.setButtonText ("Randomize");
    randomizeSeedButton.setColour (juce::TextButton::buttonColourId, kBoxBackground);
    randomizeSeedButton.setColour (juce::TextButton::textColourOffId, juce::Colours::white);
    randomizeSeedButton.onClick = [this] { audioProcessor.randomizeInstanceSeed(); };
    addAndMakeVisible (randomizeSeedButton);

    statusLabel.setJustificationType (juce::Justification::centred);
    statusLabel.setColour (juce::Label::textColourId, kMuted);
    statusLabel.setFont (juce::FontOptions (13.0f));
    addAndMakeVisible (statusLabel);

    auto& state = audioProcessor.getParameters();
    bypassAttachment = std::make_unique<ButtonAttachment> (state, "bypass", bypassButton);
    holdBarsAttachment = std::make_unique<SliderAttachment> (state, "holdBars", holdBarsSlider);
    holdBarsRandomAttachment = std::make_unique<ButtonAttachment> (state, "holdBarsRandom", holdBarsRandomButton);
    overlapModeAttachment = std::make_unique<ComboBoxAttachment> (state, "overlapMode", overlapModeBox);
    phraseGapAttachment = std::make_unique<SliderAttachment> (state, "phraseGapBeats", phraseGapSlider);
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
    instanceSeedAttachment = std::make_unique<SliderAttachment> (state, "instanceSeed", instanceSeedSlider);

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

    titleLabel.setBounds (area.removeFromTop (40));
    subtitleLabel.setBounds (area.removeFromTop (22));
    buildLabel.setBounds (area.removeFromTop (18));
    area.removeFromTop (12);

    bypassButton.setBounds (area.removeFromTop (28));
    area.removeFromTop (8);

    auto row = [&area] (int height = 28) { return area.removeFromTop (height); };

    holdBarsLabel.setBounds (row (18));
    auto holdBarsRow = row();
    holdBarsRandomButton.setBounds (holdBarsRow.removeFromRight (75));
    holdBarsRow.removeFromRight (8);
    holdBarsSlider.setBounds (holdBarsRow);
    area.removeFromTop (8);

    overlapModeLabel.setBounds (row (18));
    overlapModeBox.setBounds (row());
    area.removeFromTop (8);

    phraseGapLabel.setBounds (row (18));
    phraseGapSlider.setBounds (row());
    area.removeFromTop (8);

    restlessnessLabel.setBounds (row (18));
    auto restlessnessRow = row();
    contentAwareWeightingButton.setBounds (restlessnessRow.removeFromRight (110));
    restlessnessRow.removeFromRight (8);
    restlessnessSlider.setBounds (restlessnessRow);
    area.removeFromTop (8);

    transformLabel.setBounds (row (18));
    transformBox.setBounds (row());
    area.removeFromTop (8);

    transposeLabel.setBounds (row (18));
    auto transposeRow = row();
    transposeRandomButton.setBounds (transposeRow.removeFromRight (90));
    transposeRow.removeFromRight (8);
    transposeSlider.setBounds (transposeRow);
    area.removeFromTop (8);

    rotationLabel.setBounds (row (18));
    auto rotationRow = row();
    rotationRandomButton.setBounds (rotationRow.removeFromRight (90));
    rotationRow.removeFromRight (8);
    rotationSlider.setBounds (rotationRow);
    area.removeFromTop (8);

    lengthLabel.setBounds (row (18));
    auto lengthRow = row();
    lengthRandomButton.setBounds (lengthRow.removeFromRight (90));
    lengthRow.removeFromRight (8);
    lengthSlider.setBounds (lengthRow);
    area.removeFromTop (8);

    stretchLabel.setBounds (row (18));
    auto stretchRow = row();
    stretchQuantizedButton.setBounds (stretchRow.removeFromRight (85));
    stretchRow.removeFromRight (6);
    stretchRandomButton.setBounds (stretchRow.removeFromRight (75));
    stretchRow.removeFromRight (8);
    stretchSlider.setBounds (stretchRow);
    area.removeFromTop (8);

    intervalLabel.setBounds (row (18));
    auto intervalRow = row();
    intervalRandomButton.setBounds (intervalRow.removeFromRight (75));
    intervalRow.removeFromRight (8);
    intervalSlider.setBounds (intervalRow);
    area.removeFromTop (8);

    instanceSeedLabel.setBounds (row (18));
    auto seedRow = row();
    randomizeSeedButton.setBounds (seedRow.removeFromRight (90));
    seedRow.removeFromRight (8);
    instanceSeedSlider.setBounds (seedRow);
    area.removeFromTop (12);

    statusLabel.setBounds (row (52));
}

void OrchDelayAudioProcessorEditor::timerCallback()
{
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
                         juce::String (audioProcessor.lastResolvedStretchPercentForUi(), 1),
                         juce::dontSendNotification);
}
