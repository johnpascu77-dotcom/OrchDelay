#pragma once

#include <array>
#include <atomic>
#include <vector>
#include <JuceHeader.h>

#include "OrchDelayLogic.h"

class OrchDelayAudioProcessor final : public juce::AudioProcessor
{
public:
    OrchDelayAudioProcessor();
    ~OrchDelayAudioProcessor() override = default;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;

    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;

    void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override;

    const juce::String getName() const override;

    bool acceptsMidi() const override;
    bool producesMidi() const override;
    bool isMidiEffect() const override;
    double getTailLengthSeconds() const override;

    int getNumPrograms() override;
    int getCurrentProgram() override;
    void setCurrentProgram (int index) override;
    const juce::String getProgramName (int index) override;
    void changeProgramName (int index, const juce::String& newName) override;

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    juce::AudioProcessorValueTreeState& getParameters();
    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

    // --- UI read-only status (see OrchDelayEditor) --------------------------
    // The 3 running counters below (never reset except by prepareToPlay) are
    // a diagnostic added after two rounds of "no output, unclear why" live
    // reports - they let a silent session be told apart at a glance: MIDI
    // never arrived at all (notesCaptured stays 0) vs. arrived and was
    // buffered but never closed into a phrase (phrasesClosed stays 0) vs.
    // closed but never fired (phrasesFired stays 0, e.g. still mid-hold).
    bool hasTransportForUi() const { return haveTransportUi.load(); }
    int pendingPhraseCountForUi() const { return pendingPhraseCountUi.load(); }
    int notesCapturedForUi() const { return totalNotesCapturedUi.load(); }
    int phrasesClosedForUi() const { return totalPhrasesClosedUi.load(); }
    int phrasesFiredForUi() const { return totalPhrasesFiredUi.load(); }
    int stopEventsForUi() const { return totalStopEventsUi.load(); }
    int rewindDetectedForUi() const { return totalRewindDetectedUi.load(); }
    int rewindActedForUi() const { return totalRewindActedUi.load(); }
    double lastScheduledFirePpqForUi() const { return lastScheduledFirePpqUi.load(); }
    double furthestBlockPpqForUi() const { return furthestBlockPpqUi.load(); }
    int lastChosenTransformForUi() const { return lastChosenTransformUi.load(); }
    int lastManualChoiceForUi() const { return lastManualChoiceUi.load(); }
    float lastResolvedStretchPercentForUi() const { return lastResolvedStretchPercentUi.load(); }
    int skippedBusyForUi() const { return totalPhrasesSkippedBusyUi.load(); }
    int skippedQualityForUi() const { return totalPhrasesSkippedQualityUi.load(); }
    float lastPhraseInterestForUi() const { return lastPhraseInterestUi.load(); }
    int memoryCallbacksForUi() const { return totalMemoryCallbacksUi.load(); }

    // "Randomize" button target - writes through the normal parameter path
    // (undoable, saved in state), never a bare non-parameter side value.
    void randomizeInstanceSeed();

private:
    juce::AudioProcessorValueTreeState parameters;

    std::atomic<float>* bypassParameter = nullptr;
    std::atomic<float>* holdBarsParameter = nullptr;
    std::atomic<float>* holdBarsRandomParameter = nullptr;
    std::atomic<float>* overlapModeParameter = nullptr;
    std::atomic<float>* phraseGapBeatsParameter = nullptr;
    std::atomic<float>* restlessnessParameter = nullptr;
    std::atomic<float>* manualTransformParameter = nullptr;
    std::atomic<float>* transposeSemitonesParameter = nullptr;
    std::atomic<float>* transposeRandomParameter = nullptr;
    std::atomic<float>* rotationStepsParameter = nullptr;
    std::atomic<float>* rotationRandomParameter = nullptr;
    std::atomic<float>* lengthPercentParameter = nullptr;
    std::atomic<float>* lengthRandomParameter = nullptr;
    std::atomic<float>* stretchPercentParameter = nullptr;
    std::atomic<float>* stretchRandomParameter = nullptr;
    std::atomic<float>* stretchQuantizedParameter = nullptr;
    std::atomic<float>* intervalScalePercentParameter = nullptr;
    std::atomic<float>* intervalRandomParameter = nullptr;
    std::atomic<float>* contentAwareWeightingParameter = nullptr;
    std::atomic<float>* minimumInterestParameter = nullptr;
    std::atomic<float>* callbackProbabilityParameter = nullptr;
    std::atomic<float>* captureModeParameter = nullptr;
    std::atomic<float>* instanceSeedParameter = nullptr;

    double sampleRate = 44100.0;
    double lastBlockEndPpq = 0.0;
    bool haveLastBlockEnd = false;
    bool wasPlaying = false;

    std::atomic<bool> haveTransportUi { false };
    std::atomic<int> pendingPhraseCountUi { 0 };
    std::atomic<int> totalNotesCapturedUi { 0 };
    std::atomic<int> totalPhrasesClosedUi { 0 };
    std::atomic<int> totalPhrasesFiredUi { 0 };
    std::atomic<int> totalStopEventsUi { 0 };      // how many stoppedPlaying transitions fired
    std::atomic<int> totalRewindDetectedUi { 0 };   // `rewound` true, regardless of the playing gate
    std::atomic<int> totalRewindActedUi { 0 };      // `rewound && playing` - the purge actually ran
    std::atomic<double> lastScheduledFirePpqUi { -1.0 };   // most recent phrase's scheduledFirePpq
    std::atomic<double> furthestBlockPpqUi { -1.0 };       // furthest blockEndPpq ever seen while playing
    std::atomic<int> lastChosenTransformUi { -1 };   // odly::TransformKind actually resolved last time
    std::atomic<int> lastManualChoiceUi { -1 };      // raw manualTransform parameter value read last time
    std::atomic<float> lastResolvedStretchPercentUi { -1.0f };   // actual stretch % passed to buildOutputNotes
    std::atomic<int> totalPhrasesSkippedBusyUi { 0 };   // Overlap Mode "Skip" discards - due while busy
    std::atomic<int> totalPhrasesSkippedQualityUi { 0 };   // phrase-quality gate discards - see Docs SS20
    std::atomic<float> lastPhraseInterestUi { -1.0f };     // most recent phrase's own 0-100% interest score
    std::atomic<int> totalMemoryCallbacksUi { 0 };   // times an OLDER phrase was echoed instead - Docs SS21

    juce::int64 nextNoteSeq = 1;      // 0 never issued, matches OrchPiano's own convention
    int nextPhraseId = 0;
    int phraseCounter = 0;            // increments once per phrase closure - see odly::proposeTransform

    odly::Phrase openPhrase;
    std::vector<odly::Phrase> pendingPhrases;
    std::vector<odly::ActiveFiredNote> activeFiredNotes;

    // Most-recent-N phrases actually PLAYED (never a callback substitution
    // itself - see Docs SS21), oldest evicted first once full. Fixed pool
    // size, not exposed as a parameter in this first version.
    static constexpr int kMaxPhraseMemorySize = 8;
    std::vector<odly::MemoryEntry> phraseMemory;

    // Tracks which live notes are currently passed straight through
    // (Overlay/Duck capture modes, see Docs SS22), by [channel][pitch]
    // (channel 1-16 used, index 0 unused) - so a note's own real note-off
    // ALWAYS passes through too, once its note-on did, regardless of
    // whether the capture mode or the currently-sounding echo's pitch
    // range changed in between. Flushed (note-offs emitted AND cleared
    // together) by drainAndSilence, same stuck-note-cleanup discipline as
    // activeFiredNotes.
    std::array<std::array<bool, 128>, 17> passthroughHeld {};

    // Resolves the manual/proposed transform choice for a just-closed phrase
    // and builds its outputNotes (odly::buildOutputNotes) - each note's own
    // independent, absolute output onset/off time, computed once here so
    // firing later never re-derives the transform or bundles the phrase's
    // notes into a single block's emission. `transposeSemitones` is passed
    // in (the STOP call site needs its own local re-read, matching
    // holdBarsAtStop's own pattern); rotationSteps/lengthPercent/
    // transposeRandom are read directly from their own parameters here,
    // since none of those have that same complication. In Random Transpose
    // mode, the passed-in transposeSemitones becomes the symmetric RANGE
    // bound rather than the literal amount used - see
    // odly::resolveRandomTransposeSemitones.
    void resolveAndScheduleTransform (odly::Phrase& phrase, int transposeSemitones);

    // Shared by all 3 closure sites (capture-triggered, checkPhraseTimeout,
    // stop-triggered): checks `closed` against the Minimum Interest gate
    // (odly::computePhraseInterest, Docs SS20) and either schedules it
    // normally (resolveAndScheduleTransform + push to pendingPhrases) or
    // discards it silently, incrementing totalPhrasesSkippedQualityUi - the
    // phrase was still captured and closed (already counted in
    // totalPhrasesClosedUi by the caller), it just never gets echoed.
    void scheduleClosedPhrase (odly::Phrase closed, int transposeSemitones);

    // Silences anything currently sounding from a prior firing AND clears
    // the tracking table, in one operation - see Docs SS2's stuck-note-
    // cleanup discipline (never a "damp but don't clear" path split from a
    // differently-timed "clear" path).
    void drainAndSilence (juce::MidiBuffer& output, int samplePosition);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (OrchDelayAudioProcessor)
};
