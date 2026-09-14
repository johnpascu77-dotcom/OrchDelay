#pragma once

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

    // "Randomize" button target - writes through the normal parameter path
    // (undoable, saved in state), never a bare non-parameter side value.
    void randomizeInstanceSeed();

private:
    juce::AudioProcessorValueTreeState parameters;

    std::atomic<float>* bypassParameter = nullptr;
    std::atomic<float>* holdBarsParameter = nullptr;
    std::atomic<float>* phraseGapBeatsParameter = nullptr;
    std::atomic<float>* restlessnessParameter = nullptr;
    std::atomic<float>* manualTransformParameter = nullptr;
    std::atomic<float>* transposeSemitonesParameter = nullptr;
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

    juce::int64 nextNoteSeq = 1;      // 0 never issued, matches OrchPiano's own convention
    int nextPhraseId = 0;
    int phraseCounter = 0;            // increments once per phrase closure - see odly::proposeTransform

    odly::Phrase openPhrase;
    std::vector<odly::Phrase> pendingPhrases;
    std::vector<odly::ActiveFiredNote> activeFiredNotes;

    // Resolves the manual/proposed transform choice for a just-closed phrase
    // and applies it, producing the note list actually scheduled to fire.
    void resolveAndScheduleTransform (odly::Phrase& phrase);

    // Silences anything currently sounding from a prior firing AND clears
    // the tracking table, in one operation - see Docs SS2's stuck-note-
    // cleanup discipline (never a "damp but don't clear" path split from a
    // differently-timed "clear" path).
    void drainAndSilence (juce::MidiBuffer& output, int samplePosition);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (OrchDelayAudioProcessor)
};
