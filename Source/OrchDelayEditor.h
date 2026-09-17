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
    juce::String getLinkStatusText() const;
    void setShowingMatrix (bool shouldShow);

    // Connection Matrix (Docs SS31): read-only "who feeds whom" visualization,
    // built from each connected instance's own Broadcast/Listen Channel
    // rather than a raw channel-number grid - a cell lights up where one
    // row's Broadcast Channel matches another row's Listen Channel, which
    // reads directly as "row feeds column" using real instance labels. Only
    // ever meaningful on the hub itself (only the hub ever hears from other
    // clients at all - see OrchDelayLink::getRemoteStatusesForUi's own doc
    // comment), so the tab button that reveals it is only shown when
    // Broadcast Hub is checked.
    class ConnectionMatrixView : public juce::Component
    {
    public:
        struct Row
        {
            juce::String label;
            int broadcastChannel = 0;
            int listenChannel = 0;
            bool isSelf = false;
        };

        void setRows (std::vector<Row> newRows);
        void paint (juce::Graphics&) override;

    private:
        std::vector<Row> rows;
    };

    OrchDelayAudioProcessor& audioProcessor;

    juce::Label titleLabel;
    juce::Label subtitleLabel;
    juce::Label buildLabel;

    juce::ToggleButton bypassButton;

    juce::Label captureModeLabel;
    juce::ComboBox captureModeBox;

    juce::Label holdBarsLabel;
    juce::Slider holdBarsSlider;
    juce::ToggleButton holdBarsRandomButton;

    juce::Label overlapModeLabel;
    juce::ComboBox overlapModeBox;

    juce::Label phraseGapLabel;
    juce::Slider phraseGapSlider;

    juce::Label minimumInterestLabel;
    juce::Slider minimumInterestSlider;

    juce::Label callbackProbabilityLabel;
    juce::Slider callbackProbabilitySlider;

    juce::Label autonomousFireLabel;
    juce::Slider autonomousFireSlider;

    juce::Label captureBankLabel;
    juce::ComboBox captureBankBox;
    juce::TextButton clearBankButton;

    juce::Label activeBankLabel;
    juce::ComboBox activeBankBox;

    juce::Label recencyBiasLabel;
    juce::Slider recencyBiasSlider;

    juce::Label restlessnessLabel;
    juce::Slider restlessnessSlider;
    juce::ToggleButton contentAwareWeightingButton;

    juce::Label transformLabel;
    juce::ComboBox transformBox;

    juce::Label transposeLabel;
    juce::Slider transposeSlider;
    juce::ToggleButton transposeRandomButton;   // "Random" - see odly::resolveRandomTransposeSemitones

    juce::Label rotationLabel;
    juce::Slider rotationSlider;
    juce::ToggleButton rotationRandomButton;

    juce::Label lengthLabel;
    juce::Slider lengthSlider;
    juce::ToggleButton lengthRandomButton;

    juce::Label stretchLabel;
    juce::Slider stretchSlider;
    juce::ToggleButton stretchRandomButton;
    juce::ToggleButton stretchQuantizedButton;

    juce::Label intervalLabel;
    juce::Slider intervalSlider;
    juce::ToggleButton intervalRandomButton;

    juce::Label linkHubLabel;
    juce::ToggleButton linkHubButton;
    juce::TextButton clearRemoteButton;

    juce::Label broadcastChannelLabel;
    juce::Slider broadcastChannelSlider;

    juce::Label listenChannelLabel;
    juce::Slider listenChannelSlider;

    juce::Label instanceSeedLabel;
    juce::Slider instanceSeedSlider;
    juce::TextButton randomizeSeedButton;

    // Free-text identity (Docs SS31) - shown in the Connection Matrix instead
    // of a bare Instance Seed number. Not an APVTS parameter, see
    // OrchDelayProcessor::getInstanceLabelForUi's own doc comment - plain
    // juce::TextEditor with no ButtonAttachment/SliderAttachment, same
    // pattern OrchCapture's own free-text fields (markers/tempo/score order)
    // already use in this ecosystem.
    juce::Label instanceLabelLabel;
    juce::TextEditor instanceLabelEditor;

    // Only shown/enabled when Broadcast Hub is checked (see linkHubButton) -
    // a non-hub instance never has any remote status data to show.
    juce::TextButton matrixTabButton;
    ConnectionMatrixView matrixView;
    bool showingMatrix = false;

    // Every control that belongs to the normal parameter view (everything
    // except title/subtitle/build/status and the matrix machinery itself) -
    // toggled as a group by setShowingMatrix rather than tracked one at a
    // time, so a new control added later only needs one push_back, not a
    // second visibility-toggle site to remember.
    std::vector<juce::Component*> mainPanelComponents;

    juce::Label statusLabel;   // transport-present / pending-phrase-count, refreshed via Timer

    using ButtonAttachment = juce::AudioProcessorValueTreeState::ButtonAttachment;
    using SliderAttachment = juce::AudioProcessorValueTreeState::SliderAttachment;
    using ComboBoxAttachment = juce::AudioProcessorValueTreeState::ComboBoxAttachment;

    std::unique_ptr<ButtonAttachment> bypassAttachment;
    std::unique_ptr<ComboBoxAttachment> captureModeAttachment;
    std::unique_ptr<SliderAttachment> holdBarsAttachment;
    std::unique_ptr<ButtonAttachment> holdBarsRandomAttachment;
    std::unique_ptr<ComboBoxAttachment> overlapModeAttachment;
    std::unique_ptr<SliderAttachment> phraseGapAttachment;
    std::unique_ptr<SliderAttachment> minimumInterestAttachment;
    std::unique_ptr<SliderAttachment> callbackProbabilityAttachment;
    std::unique_ptr<SliderAttachment> autonomousFireAttachment;
    std::unique_ptr<ComboBoxAttachment> captureBankAttachment;
    std::unique_ptr<ComboBoxAttachment> activeBankAttachment;
    std::unique_ptr<SliderAttachment> recencyBiasAttachment;
    std::unique_ptr<SliderAttachment> restlessnessAttachment;
    std::unique_ptr<ButtonAttachment> contentAwareWeightingAttachment;
    std::unique_ptr<ComboBoxAttachment> transformAttachment;
    std::unique_ptr<SliderAttachment> transposeAttachment;
    std::unique_ptr<ButtonAttachment> transposeRandomAttachment;
    std::unique_ptr<SliderAttachment> rotationAttachment;
    std::unique_ptr<ButtonAttachment> rotationRandomAttachment;
    std::unique_ptr<SliderAttachment> lengthAttachment;
    std::unique_ptr<ButtonAttachment> lengthRandomAttachment;
    std::unique_ptr<SliderAttachment> stretchAttachment;
    std::unique_ptr<ButtonAttachment> stretchRandomAttachment;
    std::unique_ptr<ButtonAttachment> stretchQuantizedAttachment;
    std::unique_ptr<SliderAttachment> intervalAttachment;
    std::unique_ptr<ButtonAttachment> intervalRandomAttachment;
    std::unique_ptr<ButtonAttachment> linkHubAttachment;
    std::unique_ptr<SliderAttachment> broadcastChannelAttachment;
    std::unique_ptr<SliderAttachment> listenChannelAttachment;
    std::unique_ptr<SliderAttachment> instanceSeedAttachment;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (OrchDelayAudioProcessorEditor)
};
