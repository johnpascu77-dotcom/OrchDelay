#pragma once

#include <JuceHeader.h>
#include "OrchDelayProcessor.h"
#include "OrchDelayLink.h"   // OrchDelayLink::PresetEntry (Hub-pushed presets, SS39), OrchDelayLink::ShapeEntry (Hub General Parameters, SS45)

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

            // Opaque handle for OrchDelayLink::sendSetListenChannel - see
            // that struct field's own doc comment in OrchDelayLink.h. 0 for
            // the self row (the editor writes its own parameter directly,
            // no network round-trip needed).
            juce::int64 connectionId = 0;
        };

        // Sizes itself to its own natural content size (fixed per-cell
        // dimensions x row count, never shrunk to fit whatever's visible) and
        // relies on the owning editor's Viewport for scrolling once that
        // exceeds the visible area - see computeGridGeometry()'s own comment.
        void setRows (std::vector<Row> newRows);
        const std::vector<Row>& getRows() const { return rows; }
        void paint (juce::Graphics&) override;
        void mouseDown (const juce::MouseEvent&) override;

        // Click-to-wire (Docs SS33): fired with (sourceRowIndex,
        // destColumnIndex) when the user clicks a genuinely actionable cell
        // (off the diagonal, source row actually broadcasting on some
        // channel > 0) - never fired for a click that couldn't mean
        // anything. The editor owns turning that into an actual parameter
        // change (self vs remote destination); this view only reports
        // geometry, it never touches audioProcessor or the Link itself.
        std::function<void (int, int)> onCellClicked;

    private:
        // Shared by paint() and mouseDown() so hit-testing can never drift
        // out of sync with what's actually drawn - computed fresh each call
        // (cheap: a handful of divisions), not cached. Column width and row
        // height are deliberately independent (cellW > cellH) - columns
        // need room to show each destination's own name (not a bare index,
        // which forces cross-referencing back to the row labels - reported
        // directly by the user as the actual source of confusion), rows
        // stay compact since their labels already have the whole left
        // gutter to themselves.
        struct GridGeometry { int leftGutter, topStrip, cellW, cellH, gridX, gridY, n; };
        GridGeometry computeGridGeometry() const;

        std::vector<Row> rows;
    };

    OrchDelayAudioProcessor& audioProcessor;

    juce::Label titleLabel;
    juce::Label subtitleLabel;
    juce::Label buildLabel;

    juce::ToggleButton bypassButton;

    juce::Label captureModeLabel;
    juce::ComboBox captureModeBox;

    // Docs SS40: a keyswitch-range note should never be treated as musical
    // content to capture/replay/broadcast - the mirror image of OrchGate's
    // own "Pass Keyswitches", which always LETS a keyswitch range through
    // regardless of gate state. See createParameterLayout's own doc comment
    // in OrchDelayProcessor.cpp for the real live-rig bug this fixes.
    juce::Label ignoreKeyswitchesLabel;
    juce::ToggleButton ignoreKeyswitchesButton;
    juce::Label ksIgnoreMinLabel;
    juce::Slider ksIgnoreMinSlider;
    juce::Label ksIgnoreMaxLabel;
    juce::Slider ksIgnoreMaxSlider;

    // SS43: see monophonicCapture's own doc comment in createParameterLayout
    // for the live-rig bug this fixes (Horn 4 sounding two-note clusters,
    // relayed from a legato source with genuinely overlapping captured note
    // durations). Off by default - only turn on for an instrument modeled as
    // a monophonic player.
    juce::ToggleButton monophonicCaptureButton;

    juce::Label holdBarsLabel;
    juce::Slider holdBarsSlider;
    juce::ToggleButton holdBarsRandomButton;
    juce::Slider holdBarsRangeSlider;   // two-thumb min/max, shown instead of holdBarsSlider when Random is on (Docs SS32)

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
    juce::Slider transposeRangeSlider;

    juce::Label rotationLabel;
    juce::Slider rotationSlider;
    juce::ToggleButton rotationRandomButton;
    juce::Slider rotationRangeSlider;

    juce::Label lengthLabel;
    juce::Slider lengthSlider;
    juce::ToggleButton lengthRandomButton;
    juce::Slider lengthRangeSlider;

    juce::Label stretchLabel;
    juce::Slider stretchSlider;
    juce::ToggleButton stretchRandomButton;
    juce::ToggleButton stretchQuantizedButton;
    juce::Slider stretchRangeSlider;

    juce::Label intervalLabel;
    juce::Slider intervalSlider;
    juce::ToggleButton intervalRandomButton;
    juce::Slider intervalRangeSlider;

    juce::Label linkHubLabel;
    juce::ToggleButton linkHubButton;
    juce::TextButton clearRemoteButton;

    juce::Label broadcastChannelLabel;
    juce::Slider broadcastChannelSlider;

    juce::Label listenChannelLabel;
    juce::Slider listenChannelSlider;

    // SS42: the real MIDI channel every note THIS instance FIRES (local-bank
    // replay, Autonomous Fire, relayed Remote-bank content alike) actually
    // goes out on - auto-detected from real incoming note-ons by default (0),
    // override only if auto-detect proves flaky for an instrument with
    // sparse/no live input (same caveat as OrchNoteMapper's own KS Generator
    // Channel override). See outputChannelOverride's own doc comment in
    // createParameterLayout for the real live-rig bug (Horn 4 polyphony,
    // 2026-09-28) this fixes.
    juce::Label outputChannelLabel;
    juce::Slider outputChannelSlider;

    // Off by default (Docs SS35) - re-broadcasts Remote-bank material this
    // instance fires via Autonomous Fire, on its own Broadcast Channel
    // above, tagged with an incremented hop count. What actually lets a
    // chain of instances pass material along past a node with no live MIDI
    // of its own, rather than each one only ever playing back what it
    // directly received.
    juce::ToggleButton relayEnabledButton;

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
    juce::Viewport matrixViewport;   // matrixView sizes itself to full content; this scrolls it when that exceeds the visible area (2026-09-26: a real rig runs ~44 instances, far more than fit unscrolled)
    bool showingMatrix = false;

    // SS44 (2026-09-28): the main parameter view grew past a screen's worth
    // of height across this whole session's feature growth (Hub Presets,
    // Ignore Keyswitches, Reset All Instances, Output Channel override,
    // Monophonic Capture - each one bumping the WINDOW's own fixed height a
    // little further, see the constructor's own comment on setSize) - a real
    // live report confirmed the window now simply runs off a real screen
    // with no way to see the rest. Same fix as matrixViewport/matrixView's
    // own established pattern just above: mainPanelContent holds every
    // control the main view has always had (moved into it at construction
    // time, see the reparenting block at the end of the constructor - the
    // 60+ addAndMakeVisible/addChildComponent call sites elsewhere in this
    // file are UNCHANGED), sized to a fixed height generous enough to never
    // clip; mainPanelViewport scrolls it when the window is shorter than
    // that, exactly like matrixViewport already does for matrixView. Fills
    // the same fullWidthArea matrixViewport does - only one of the two is
    // ever visible at a time, same as before.
    juce::Component mainPanelContent;
    juce::Viewport mainPanelViewport;

    // Hub-pushed presets (SS39, Docs SS39): a routing snapshot (Broadcast/
    // Listen Channel, Relay Remote Material, Active Bank) for every instance
    // in the rig, captured from the SAME live heartbeat table the Connection
    // Matrix reads, save/loadable to a JSON file on disk, and re-sendable
    // on demand - the actual fix for a whole session's worth of "one
    // instance's routing silently reverted and now the chain is broken"
    // (2026-09-27). Only shown/enabled when Broadcast Hub is checked, same
    // as the Matrix - only the hub ever has a live table of other instances
    // to capture from. `pendingPresetEntries` is deliberately a hold buffer,
    // not auto-sent - Save/Load only ever populate it and report a count;
    // Send Preset is the one and only action that actually touches the
    // rest of the rig, so a misclicked file dialog can never silently push
    // a stale/wrong routing table onto 27 other live instances.
    juce::Label presetSectionLabel;
    juce::TextButton savePresetButton;
    juce::TextButton loadPresetButton;
    juce::TextButton sendPresetButton;
    juce::Label presetStatusLabel;
    std::vector<OrchDelayLink::PresetEntry> pendingPresetEntries;
    std::unique_ptr<juce::FileChooser> presetFileChooser;   // kept alive for the duration of an async chooser
    void capturePresetToBuffer();
    void loadPresetFromFile (const juce::File&);
    void savePresetToFile (const juce::File&);
    void sendPendingPreset();

    // Hub-pushed full reset (SS41): the rig-wide "start completely fresh"
    // Reset All Instances button - one click, from wherever the Hub happens
    // to be, instead of Clear Bank (x3) + Clear Remote on every single
    // instance by hand. Same isHub-only visibility as the preset section
    // above (only the hub has any live connections to fan a reset out to),
    // deliberately kept in its own row rather than folded into the preset
    // buttons - it wipes musical content, not routing, and shouldn't read as
    // "just another preset action."
    juce::TextButton resetAllButton;

    // SS45: Hub "General Parameters" - a shared CONTENT-SHAPING template
    // (see OrchDelayLink::ShapeEntry's own doc comment) authored once here on
    // the Hub's own sliders/toggles and pushed to the whole rig - the
    // "control tower" the Hub Presets section above already is for ROUTING,
    // now for the musical timing/character knobs that were being re-clicked
    // ad hoc per instance instead. These controls are DELIBERATELY plain
    // (no SliderAttachment/ButtonAttachment) - they represent a value to
    // SEND, not this instance's own current setting, same reasoning
    // pendingPresetEntries above has for staying a hold buffer until Send is
    // actually clicked. Same isHub-only visibility as the sections above.
    juce::Label shapeSectionLabel;
    juce::Label shapeHoldBarsLabel;
    juce::Slider shapeHoldBarsSlider;
    juce::Label shapeAutonomousFireLabel;
    juce::Slider shapeAutonomousFireSlider;
    juce::Label shapePhraseGapLabel;
    juce::Slider shapePhraseGapSlider;
    juce::Label shapeMinimumInterestLabel;
    juce::Slider shapeMinimumInterestSlider;
    juce::Label shapeCallbackProbabilityLabel;
    juce::Slider shapeCallbackProbabilitySlider;
    juce::ToggleButton shapeMonophonicCaptureButton;
    juce::ToggleButton shapeIgnoreKeyswitchesButton;
    juce::TextButton sendShapeButton;
    juce::TextButton randomizeSendShapeButton;
    OrchDelayLink::ShapeEntry buildShapeFromUi() const;

    // Every control that belongs to the normal parameter view (everything
    // except title/subtitle/build/status and the matrix machinery itself) -
    // toggled as a group by setShowingMatrix rather than tracked one at a
    // time, so a new control added later only needs one push_back, not a
    // second visibility-toggle site to remember.
    std::vector<juce::Component*> mainPanelComponents;

    // Random-range two-thumb sliders (Docs SS32): each of the 6 Random-
    // capable parameters (Hold Bars, Transpose, Rotation, Length, Stretch,
    // Interval Scale) has its own dedicated Min/Max APVTS parameter pair,
    // used only when that parameter's own "Random" toggle is on. A
    // TwoValueHorizontal juce::Slider has no AudioProcessorValueTreeState
    // attachment helper (attachments are single-value only), so each is
    // wired by hand: onValueChange below pushes the two thumb positions
    // into the Min/Max parameters (normalised via convertTo0to1, since
    // RangedAudioParameter::setValueNotifyingHost expects 0-1, not the real
    // units), and timerCallback below pulls the current parameter values
    // back into the slider's own display whenever it isn't actively being
    // dragged (so an undo, a project reload, or a Randomize-style external
    // change is reflected without a second, parallel sync mechanism).
    // Occupies the exact same layout slot as its corresponding manual
    // slider, shown instead of it (never alongside) when Random is on -
    // see setupRandomRangeBinding and the visibility pass in timerCallback.
    struct RandomRangeBinding
    {
        juce::Slider* rangeSlider = nullptr;
        juce::Slider* manualSlider = nullptr;
        juce::ToggleButton* randomButton = nullptr;
        juce::Label* label = nullptr;
        juce::String baseLabelText;   // restored to the label when Random is off
        bool isPercent = true;        // display formatting: "50.0%" vs a plain integer
        std::atomic<float>* minParamRaw = nullptr;
        std::atomic<float>* maxParamRaw = nullptr;
        juce::RangedAudioParameter* minParam = nullptr;
        juce::RangedAudioParameter* maxParam = nullptr;
    };
    std::vector<RandomRangeBinding> randomRangeBindings;
    void setupRandomRangeBinding (juce::Slider& rangeSlider, juce::Slider& manualSlider,
                                  juce::ToggleButton& randomButton, juce::Label& label,
                                  bool isPercent,
                                  const juce::String& minParamId, const juce::String& maxParamId);

    juce::Label statusLabel;   // transport-present / pending-phrase-count, refreshed via Timer

    using ButtonAttachment = juce::AudioProcessorValueTreeState::ButtonAttachment;
    using SliderAttachment = juce::AudioProcessorValueTreeState::SliderAttachment;
    using ComboBoxAttachment = juce::AudioProcessorValueTreeState::ComboBoxAttachment;

    std::unique_ptr<ButtonAttachment> bypassAttachment;
    std::unique_ptr<ComboBoxAttachment> captureModeAttachment;
    std::unique_ptr<ButtonAttachment> ignoreKeyswitchesAttachment;
    std::unique_ptr<ButtonAttachment> monophonicCaptureAttachment;
    std::unique_ptr<SliderAttachment> ksIgnoreMinAttachment;
    std::unique_ptr<SliderAttachment> ksIgnoreMaxAttachment;
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
    std::unique_ptr<SliderAttachment> outputChannelAttachment;
    std::unique_ptr<ButtonAttachment> relayEnabledAttachment;
    std::unique_ptr<SliderAttachment> instanceSeedAttachment;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (OrchDelayAudioProcessorEditor)
};
