#pragma once

#include <JuceHeader.h>
#include "OrchDelayProcessor.h"

// Minimal v1 editor: parameter controls + a one-line transport-status label
// (per Docs SS7 - a pending-phrase-queue visualization is a nice-to-have,
// not blocking for shipping and testing v1).
class OrchDelayAudioProcessorEditor : public juce::AudioProcessorEditor,
                                      private juce::Timer
{
public:
    explicit OrchDelayAudioProcessorEditor (OrchDelayAudioProcessor&);
    ~OrchDelayAudioProcessorEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override;

    OrchDelayAudioProcessor& audioProcessor;

    juce::Label titleLabel;
    juce::Label subtitleLabel;
    juce::Label buildLabel;

    juce::ToggleButton bypassButton;

    juce::Label holdBarsLabel;
    juce::Slider holdBarsSlider;

    juce::Label phraseGapLabel;
    juce::Slider phraseGapSlider;

    juce::Label restlessnessLabel;
    juce::Slider restlessnessSlider;

    juce::Label transformLabel;
    juce::ComboBox transformBox;

    juce::Label transposeLabel;
    juce::Slider transposeSlider;
    juce::ToggleButton transposeRandomButton;   // "Random" - see odly::resolveRandomTransposeSemitones

    juce::Label rotationLabel;
    juce::Slider rotationSlider;

    juce::Label lengthLabel;
    juce::Slider lengthSlider;

    juce::Label instanceSeedLabel;
    juce::Slider instanceSeedSlider;
    juce::TextButton randomizeSeedButton;

    juce::Label statusLabel;   // transport-present / pending-phrase-count, refreshed via Timer

    using ButtonAttachment = juce::AudioProcessorValueTreeState::ButtonAttachment;
    using SliderAttachment = juce::AudioProcessorValueTreeState::SliderAttachment;
    using ComboBoxAttachment = juce::AudioProcessorValueTreeState::ComboBoxAttachment;

    std::unique_ptr<ButtonAttachment> bypassAttachment;
    std::unique_ptr<SliderAttachment> holdBarsAttachment;
    std::unique_ptr<SliderAttachment> phraseGapAttachment;
    std::unique_ptr<SliderAttachment> restlessnessAttachment;
    std::unique_ptr<ComboBoxAttachment> transformAttachment;
    std::unique_ptr<SliderAttachment> transposeAttachment;
    std::unique_ptr<ButtonAttachment> transposeRandomAttachment;
    std::unique_ptr<SliderAttachment> rotationAttachment;
    std::unique_ptr<SliderAttachment> lengthAttachment;
    std::unique_ptr<SliderAttachment> instanceSeedAttachment;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (OrchDelayAudioProcessorEditor)
};
