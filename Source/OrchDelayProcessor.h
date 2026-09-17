#pragma once

#include <array>
#include <atomic>
#include <memory>
#include <mutex>
#include <vector>
#include <JuceHeader.h>

#include "OrchDelayLogic.h"

class OrchDelayLink;

class OrchDelayAudioProcessor final : public juce::AudioProcessor
{
public:
    OrchDelayAudioProcessor();
    ~OrchDelayAudioProcessor() override;

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
    int autonomousFiresForUi() const { return totalAutonomousFiresUi.load(); }
    int remoteReceivedForUi() const { return totalRemoteReceivedUi.load(); }

    // --- Cross-instance phrase broadcast (see OrchDelayLink / Docs SS27) ---
    // `link` itself exposes Hub/Client mode + connected-state for the editor
    // status line (audioProcessor.getLink().getModeForUi(), etc.) - these are
    // just the processor-side pieces OrchDelayLink's own background thread
    // needs to poll/feed, mirroring OrchCaptureLink's own established split.
    OrchDelayLink& getLink() { return *link; }
    bool isLinkHubParamOn() const { return linkHubParameter != nullptr && linkHubParameter->load() > 0.5f; }
    int getBroadcastChannelForUi() const
    {
        return broadcastChannelParameter != nullptr
            ? juce::jlimit (0, 8, juce::roundToInt (broadcastChannelParameter->load())) : 0;
    }
    int getListenChannelForUi() const
    {
        return listenChannelParameter != nullptr
            ? juce::jlimit (0, 8, juce::roundToInt (listenChannelParameter->load())) : 0;
    }
    // Bumped once (audio thread) every time ANY phrase is newly captured,
    // regardless of Broadcast Channel - OrchDelayLink's own worker thread
    // polls this the same way OrchCaptureLink polls its own take-generation
    // counter, to notice "there's a new phrase to publish" without needing
    // the audio thread to push anything across threads itself.
    int getCapturedGenerationForUi() const { return capturedGenerationUi.load(); }
    // Thread-safe copy of the most recently captured phrase, for
    // OrchDelayLink's worker thread to read (blocking lock is fine there -
    // it is NOT the audio thread). Written non-blocking (try_lock) from the
    // audio thread right after every capture - see scheduleClosedPhrase.
    odly::MemoryEntry snapshotLastCapturedForBroadcast() const;
    // Called from OrchDelayLink's own connection/worker thread (never the
    // audio thread) whenever a phrase arrives from ANOTHER instance matching
    // this instance's own Listen Channel. Queues it (blocking lock is fine
    // here, same reasoning as above); processBlock() folds the queue into
    // the Remote bank on the audio thread, reassigning fresh local `seq`
    // values first (see Docs SS27 - never trust another instance's own seq
    // numbering).
    void pushIncomingRemotePhrase (const odly::MemoryEntry& entry);

    // Free-text identity shown in the Connection Matrix (Docs SS31) instead
    // of a bare Instance Seed number - not an APVTS parameter (no clean
    // free-text parameter shape, same reasoning as randomizeInstanceSeed
    // below), so it needs its own small thread-safe storage: read by
    // OrchDelayLink's own worker thread (building each heartbeat, blocking
    // lock fine there) and written by the editor (message thread) whenever
    // the user types. Persisted as a plain XML attribute alongside the APVTS
    // state in get/setStateInformation, same trick used elsewhere in JUCE
    // for a value that doesn't fit the parameter system.
    juce::String getInstanceLabelForUi() const;
    void setInstanceLabel (const juce::String& label);

    // "Randomize" button target - writes through the normal parameter path
    // (undoable, saved in state), never a bare non-parameter side value.
    void randomizeInstanceSeed();

    // "Clear Bank" button target - clears whichever bank Capture Bank is
    // CURRENTLY set to, so a fresh idea/motive can start without old
    // material still bleeding in via Callback/Autonomous Fire (see Docs
    // SS25). Not a normal parameter (there's no clean "momentary trigger"
    // shape in APVTS, same reasoning as Randomize above), and - unlike
    // Randomize, which only ever touches its own atomic parameter float -
    // this reaches into `phraseMemoryBanks`, a plain member vector the
    // AUDIO thread also reads/writes every block, so a direct clear() here
    // on the message thread would be a real data race. Instead this just
    // raises a flag; processBlock() itself performs the actual clear, on
    // the audio thread, at the top of the next block.
    void requestClearCaptureBank();

    // Same pattern as requestClearCaptureBank above, but always targets the
    // Remote bank specifically (see Docs SS27) regardless of Capture Bank's
    // own selection - Remote is populated exclusively by incoming broadcasts
    // from OTHER instances, never by this instance's own local playing, so
    // it needed its own independent clear trigger rather than overloading
    // Capture Bank's selector with a 4th choice that could never actually be
    // captured INTO.
    void requestClearRemoteBank();

private:
    juce::AudioProcessorValueTreeState parameters;

    std::atomic<float>* bypassParameter = nullptr;
    std::atomic<float>* holdBarsParameter = nullptr;
    std::atomic<float>* holdBarsRandomParameter = nullptr;
    std::atomic<float>* holdBarsRandomMinParameter = nullptr;
    std::atomic<float>* holdBarsRandomMaxParameter = nullptr;
    std::atomic<float>* overlapModeParameter = nullptr;
    std::atomic<float>* phraseGapBeatsParameter = nullptr;
    std::atomic<float>* restlessnessParameter = nullptr;
    std::atomic<float>* manualTransformParameter = nullptr;
    std::atomic<float>* transposeSemitonesParameter = nullptr;
    std::atomic<float>* transposeRandomParameter = nullptr;
    std::atomic<float>* transposeRandomMinParameter = nullptr;
    std::atomic<float>* transposeRandomMaxParameter = nullptr;
    std::atomic<float>* rotationStepsParameter = nullptr;
    std::atomic<float>* rotationRandomParameter = nullptr;
    std::atomic<float>* rotationRandomMinParameter = nullptr;
    std::atomic<float>* rotationRandomMaxParameter = nullptr;
    std::atomic<float>* lengthPercentParameter = nullptr;
    std::atomic<float>* lengthRandomParameter = nullptr;
    std::atomic<float>* lengthRandomMinParameter = nullptr;
    std::atomic<float>* lengthRandomMaxParameter = nullptr;
    std::atomic<float>* stretchPercentParameter = nullptr;
    std::atomic<float>* stretchRandomParameter = nullptr;
    std::atomic<float>* stretchRandomMinParameter = nullptr;
    std::atomic<float>* stretchRandomMaxParameter = nullptr;
    std::atomic<float>* stretchQuantizedParameter = nullptr;
    std::atomic<float>* intervalScalePercentParameter = nullptr;
    std::atomic<float>* intervalRandomParameter = nullptr;
    std::atomic<float>* intervalRandomMinParameter = nullptr;
    std::atomic<float>* intervalRandomMaxParameter = nullptr;
    std::atomic<float>* contentAwareWeightingParameter = nullptr;
    std::atomic<float>* minimumInterestParameter = nullptr;
    std::atomic<float>* callbackProbabilityParameter = nullptr;
    std::atomic<float>* captureModeParameter = nullptr;
    std::atomic<float>* autonomousFireBarsParameter = nullptr;
    std::atomic<float>* captureBankParameter = nullptr;
    std::atomic<float>* activeBankParameter = nullptr;
    std::atomic<float>* recencyBiasParameter = nullptr;
    std::atomic<float>* linkHubParameter = nullptr;
    std::atomic<float>* broadcastChannelParameter = nullptr;
    std::atomic<float>* listenChannelParameter = nullptr;
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
    std::atomic<int> totalAutonomousFiresUi { 0 };   // times the device fired on its own clock - Docs SS24
    std::atomic<int> totalRemoteReceivedUi { 0 };    // phrases folded into the Remote bank - Docs SS27
    std::atomic<int> capturedGenerationUi { 0 };     // bumped on every capture - OrchDelayLink polls this

    juce::int64 nextNoteSeq = 1;      // 0 never issued, matches OrchPiano's own convention
    int nextPhraseId = 0;
    int phraseCounter = 0;            // increments once per phrase closure - see odly::proposeTransform

    odly::Phrase openPhrase;
    std::vector<odly::Phrase> pendingPhrases;
    std::vector<odly::ActiveFiredNote> activeFiredNotes;

    // Overlap Mode "Wait"/"Skip" busy-gating (see Docs SS17, SS29) - the ppq
    // at which the LAST currently-started phrase's own LAST note finishes,
    // spanning that phrase's own internal rests. Deliberately NOT the same
    // thing as activeFiredNotes being non-empty (which only reflects notes
    // audibly sounding THIS instant): a monophonic phrase with a rest
    // between two of its own notes would read as "not busy" mid-rest under
    // that check, letting an unrelated Autonomous-Fire-drawn phrase start
    // in the gap - genuine cross-phrase overlap despite Wait being active,
    // even though the first phrase hadn't actually finished. Extended
    // (never shrunk) the moment ANY phrase is released/starts, to the max
    // outputOffPpq across its own outputNotes; the busy check compares this
    // against the current block instead of activeFiredNotes.empty().
    double busyUntilPpq = -1.0;

    // 4 independent memory banks - A/B/C (Docs SS25) plus Remote (Docs SS27,
    // index kRemoteBankIndex), each holding the most-recent-N phrases, oldest
    // evicted first once full. `captureBank` (3-choice, A/B/C only - Remote
    // is never a valid capture target, see requestClearRemoteBank's own doc
    // comment) selects which bank newly-closed LOCAL phrases are added to;
    // `activeBank` (4-choice, A/B/C/Remote) selects which bank Callback
    // Probability and Autonomous Fire both draw FROM - deliberately
    // independent selectors, not one shared knob, so a new bank can be
    // filled with fresh material while a different one keeps playing, then
    // switched over ("recalled") when ready - the development-section
    // workflow this was built for. Remote is populated exclusively by
    // OrchDelayLink folding in phrases received from OTHER instances.
    static constexpr int kMaxPhraseMemorySize = 8;
    static constexpr int kCaptureBankChoiceCount = 3;    // A, B, C - Capture Bank's own valid range
    static constexpr int kPhraseMemoryBankCount = 4;     // A, B, C, Remote - Active Bank's own valid range
    static constexpr int kRemoteBankIndex = kPhraseMemoryBankCount - 1;
    std::array<std::vector<odly::MemoryEntry>, kPhraseMemoryBankCount> phraseMemoryBanks;

    // Set by requestClearCaptureBank() (message thread), consumed at the
    // top of the next processBlock() (audio thread) - see that method's
    // own doc comment for why a direct clear from the UI would be unsafe.
    std::atomic<bool> clearCaptureBankRequested { false };
    std::atomic<bool> clearRemoteBankRequested { false };   // requestClearRemoteBank's own flag - Docs SS27

    // --- Cross-instance phrase broadcast plumbing (see OrchDelayLink / Docs
    // SS27) - two independent small mutex-guarded handoffs, one per
    // direction, both following the exact non-blocking-from-the-audio-thread
    // discipline OrchMergeLink already established in this ecosystem
    // (try_lock; skip and retry next time on contention, never block real-
    // time processing for IPC's sake):
    //
    // OUTGOING: scheduleClosedPhrase copies the just-captured entry here
    // (try_lock) every time ANY phrase closes; OrchDelayLink's own worker
    // thread later reads it via snapshotLastCapturedForBroadcast() (a
    // blocking lock, safe there - it is not the audio thread) once it
    // notices capturedGenerationUi has moved.
    mutable std::mutex lastCapturedMutex;
    odly::MemoryEntry lastCapturedEntry;

    // INCOMING: pushIncomingRemotePhrase() (called from OrchDelayLink's own
    // connection thread, blocking lock fine there) appends here; the top of
    // processBlock() drains it (try_lock) into phraseMemoryBanks[kRemoteBankIndex],
    // reassigning fresh local `seq` values first.
    std::mutex remoteInboxMutex;
    std::vector<odly::MemoryEntry> remoteInboxPending;

    // Connection Matrix identity (Docs SS31) - see getInstanceLabelForUi's
    // own doc comment above for why this needs its own mutex rather than
    // living as a plain member.
    mutable std::mutex instanceLabelMutex;
    juce::String instanceLabel;

    std::unique_ptr<OrchDelayLink> link;

    // Tracks which live notes are currently passed straight through
    // (Overlay/Duck capture modes, see Docs SS22), by [channel][pitch]
    // (channel 1-16 used, index 0 unused) - so a note's own real note-off
    // ALWAYS passes through too, once its note-on did, regardless of
    // whether the capture mode or the currently-sounding echo's pitch
    // range changed in between. Flushed (note-offs emitted AND cleared
    // together) by drainAndSilence, same stuck-note-cleanup discipline as
    // activeFiredNotes.
    std::array<std::array<bool, 128>, 17> passthroughHeld {};

    // Autonomous Fire (see Docs SS24): lets the device fire from its own
    // memory pool on its own clock, entirely independent of new incoming
    // MIDI, once seeded with at least one real captured phrase.
    bool autonomousFireArmed = false;
    double nextAutonomousFirePpq = 0.0;
    int autonomousFireCounter = 0;   // separate from phraseCounter - see resolveAutonomousFireIndex's own doc

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

    // Checked once per playing block, after the normal capture/closure
    // logic: if Autonomous Fire is on (autonomousFireBars > 0) and the
    // memory pool has at least one entry, arms a self-sustaining "every N
    // bars" clock on first use, then fires a pool phrase each time it
    // ticks - entirely independent of whether anything new has arrived.
    // See Docs SS24 (mechanics) and SS28 (the phase-locked re-arm fix - no
    // blockStartPpq parameter needed here since SS28: the fire anchor is now
    // the precise due ppq itself, never a block boundary).
    void checkAutonomousFire (double blockEndPpq, double beatsPerBarNow, int transposeSemitones);

    // Silences anything currently sounding from a prior firing AND clears
    // the tracking table, in one operation - see Docs SS2's stuck-note-
    // cleanup discipline (never a "damp but don't clear" path split from a
    // differently-timed "clear" path).
    void drainAndSilence (juce::MidiBuffer& output, int samplePosition);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (OrchDelayAudioProcessor)
};
