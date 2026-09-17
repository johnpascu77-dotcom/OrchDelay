#include "OrchDelayProcessor.h"
#include "OrchDelayEditor.h"
#include "OrchDelayLink.h"

#include <algorithm>

namespace
{
    // Same 0.5-beat guard band OrchCapture uses to tell a genuine backward
    // transport jump (loop/relocate) apart from ordinary block-to-block
    // forward progress.
    constexpr double kRewindBeatsGuard = 0.5;
}

OrchDelayAudioProcessor::OrchDelayAudioProcessor()
    : AudioProcessor (BusesProperties()),
      parameters (*this, nullptr, "OrchDelayParameters", createParameterLayout())
{
    bypassParameter = parameters.getRawParameterValue ("bypass");
    holdBarsParameter = parameters.getRawParameterValue ("holdBars");
    holdBarsRandomParameter = parameters.getRawParameterValue ("holdBarsRandom");
    overlapModeParameter = parameters.getRawParameterValue ("overlapMode");
    phraseGapBeatsParameter = parameters.getRawParameterValue ("phraseGapBeats");
    restlessnessParameter = parameters.getRawParameterValue ("restlessness");
    manualTransformParameter = parameters.getRawParameterValue ("manualTransform");
    transposeSemitonesParameter = parameters.getRawParameterValue ("transposeSemitones");
    transposeRandomParameter = parameters.getRawParameterValue ("transposeRandom");
    rotationStepsParameter = parameters.getRawParameterValue ("rotationSteps");
    rotationRandomParameter = parameters.getRawParameterValue ("rotationRandom");
    lengthPercentParameter = parameters.getRawParameterValue ("lengthPercent");
    lengthRandomParameter = parameters.getRawParameterValue ("lengthRandom");
    stretchPercentParameter = parameters.getRawParameterValue ("stretchPercent");
    stretchRandomParameter = parameters.getRawParameterValue ("stretchRandom");
    stretchQuantizedParameter = parameters.getRawParameterValue ("stretchQuantized");
    intervalScalePercentParameter = parameters.getRawParameterValue ("intervalScalePercent");
    intervalRandomParameter = parameters.getRawParameterValue ("intervalRandom");
    contentAwareWeightingParameter = parameters.getRawParameterValue ("contentAwareWeighting");
    minimumInterestParameter = parameters.getRawParameterValue ("minimumInterest");
    callbackProbabilityParameter = parameters.getRawParameterValue ("callbackProbability");
    captureModeParameter = parameters.getRawParameterValue ("captureMode");
    autonomousFireBarsParameter = parameters.getRawParameterValue ("autonomousFireBars");
    captureBankParameter = parameters.getRawParameterValue ("captureBank");
    activeBankParameter = parameters.getRawParameterValue ("activeBank");
    recencyBiasParameter = parameters.getRawParameterValue ("recencyBias");
    linkHubParameter = parameters.getRawParameterValue ("linkHub");
    broadcastChannelParameter = parameters.getRawParameterValue ("broadcastChannel");
    listenChannelParameter = parameters.getRawParameterValue ("listenChannel");
    instanceSeedParameter = parameters.getRawParameterValue ("instanceSeed");

    link = std::make_unique<OrchDelayLink> (*this);
}

OrchDelayAudioProcessor::~OrchDelayAudioProcessor()
{
    link.reset();
}

void OrchDelayAudioProcessor::prepareToPlay (double newSampleRate, int samplesPerBlock)
{
    juce::ignoreUnused (samplesPerBlock);
    sampleRate = newSampleRate;

    openPhrase = odly::Phrase {};
    pendingPhrases.clear();
    activeFiredNotes.clear();
    busyUntilPpq = -1.0;
    for (auto& bank : phraseMemoryBanks)
        bank.clear();
    clearCaptureBankRequested.store (false);
    clearRemoteBankRequested.store (false);
    {
        std::lock_guard<std::mutex> lock (remoteInboxMutex);
        remoteInboxPending.clear();
    }
    capturedGenerationUi.store (0);
    for (auto& channelRow : passthroughHeld)
        channelRow.fill (false);
    autonomousFireArmed = false;
    nextAutonomousFirePpq = 0.0;
    autonomousFireCounter = 0;
    nextNoteSeq = 1;
    nextPhraseId = 0;
    phraseCounter = 0;
    lastBlockEndPpq = 0.0;
    haveLastBlockEnd = false;
    wasPlaying = false;
    pendingPhraseCountUi.store (0);
    totalNotesCapturedUi.store (0);
    totalPhrasesClosedUi.store (0);
    totalPhrasesFiredUi.store (0);
    totalStopEventsUi.store (0);
    totalRewindDetectedUi.store (0);
    totalRewindActedUi.store (0);
    lastScheduledFirePpqUi.store (-1.0);
    furthestBlockPpqUi.store (-1.0);
    lastChosenTransformUi.store (-1);
    lastManualChoiceUi.store (-1);
    lastResolvedStretchPercentUi.store (-1.0f);
    totalPhrasesSkippedBusyUi.store (0);
    totalPhrasesSkippedQualityUi.store (0);
    lastPhraseInterestUi.store (-1.0f);
    totalMemoryCallbacksUi.store (0);
    totalAutonomousFiresUi.store (0);
    totalRemoteReceivedUi.store (0);
}

void OrchDelayAudioProcessor::releaseResources()
{
}

bool OrchDelayAudioProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    juce::ignoreUnused (layouts);
    return true;
}

void OrchDelayAudioProcessor::drainAndSilence (juce::MidiBuffer& output, int samplePosition)
{
    // Both halves of the discipline together, always - see Docs SS2 and this
    // function's own header comment: never emit note-offs without clearing
    // the tracking table in the same operation, and never clear it without
    // emitting the note-offs first.
    for (const auto& active : activeFiredNotes)
        output.addEvent (juce::MidiMessage::noteOff (active.channel, active.pitch), samplePosition);
    activeFiredNotes.clear();
    busyUntilPpq = -1.0;

    // Same discipline for any live note currently passed straight through
    // (Overlay/Duck capture modes, see Docs SS22) - never leave one
    // sounding downstream with no note-off ever coming.
    for (int ch = 1; ch <= 16; ++ch)
        for (int pitch = 0; pitch < 128; ++pitch)
            if (passthroughHeld[static_cast<size_t> (ch)][static_cast<size_t> (pitch)])
            {
                output.addEvent (juce::MidiMessage::noteOff (ch, pitch), samplePosition);
                passthroughHeld[static_cast<size_t> (ch)][static_cast<size_t> (pitch)] = false;
            }
}

void OrchDelayAudioProcessor::resolveAndScheduleTransform (odly::Phrase& phrase, int transposeSemitones)
{
    const float restlessness = restlessnessParameter != nullptr ? restlessnessParameter->load() : 0.0f;
    const int manualChoice = manualTransformParameter != nullptr
        ? juce::roundToInt (manualTransformParameter->load()) : 0;
    const int instanceSeed = instanceSeedParameter != nullptr
        ? juce::jlimit (0, 127, juce::roundToInt (instanceSeedParameter->load())) : 0;
    const bool transposeRandom = transposeRandomParameter != nullptr && transposeRandomParameter->load() >= 0.5f;
    const bool rotationRandom = rotationRandomParameter != nullptr && rotationRandomParameter->load() >= 0.5f;
    const bool lengthRandom = lengthRandomParameter != nullptr && lengthRandomParameter->load() >= 0.5f;
    const bool stretchRandom = stretchRandomParameter != nullptr && stretchRandomParameter->load() >= 0.5f;
    const bool stretchQuantized = stretchQuantizedParameter != nullptr && stretchQuantizedParameter->load() >= 0.5f;
    const bool intervalRandom = intervalRandomParameter != nullptr && intervalRandomParameter->load() >= 0.5f;
    const int rotationSteps = rotationStepsParameter != nullptr
        ? juce::roundToInt (rotationStepsParameter->load()) : 0;
    const float lengthPercent = lengthPercentParameter != nullptr
        ? juce::jlimit (0.0f, 100.0f, lengthPercentParameter->load()) : 100.0f;
    const float stretchPercent = stretchPercentParameter != nullptr
        ? juce::jlimit (25.0f, 400.0f, stretchPercentParameter->load()) : 100.0f;
    const float intervalScalePercent = intervalScalePercentParameter != nullptr
        ? juce::jlimit (0.0f, 300.0f, intervalScalePercentParameter->load()) : 150.0f;

    // Choice indices: 0=Follow Restlessness, 1=None, 2=Transpose, 3=Retrograde,
    // 4=Inversion, 5=Rotation, 6=Length, 7=M7, 8=Stretch, 9=Interval - any
    // explicit choice (>0) always wins outright, no blending with the
    // proposal mechanism (see this header's own doc comment).
    if (manualChoice > 0)
        phrase.chosenTransform = manualChoice - 1;   // maps directly onto odly::TransformKind
    else
    {
        // Content-aware weighting (see Docs SS19, default on) biases WHICH
        // transform gets picked by the phrase's own features - a dense
        // phrase leans toward Length, a sparse one toward Stretch, a
        // wide-range one toward Inversion/Interval - rather than a flat
        // 1-in-8 chance. Off falls back to the original equal-weight pick.
        const bool contentAwareWeighting = contentAwareWeightingParameter != nullptr
            && contentAwareWeightingParameter->load() >= 0.5f;
        const auto proposal = contentAwareWeighting
            ? odly::proposeWeightedTransform (instanceSeed, phraseCounter, restlessness,
                                              odly::computePhraseFeatures (phrase.notes))
            : odly::proposeTransform (instanceSeed, phraseCounter, restlessness);
        phrase.chosenTransform = proposal.applyAny ? proposal.transformKind : odly::kTransformNone;
    }

    lastManualChoiceUi.store (manualChoice);
    lastChosenTransformUi.store (phrase.chosenTransform);

    // Random modes: the passed-in/read parameter values become RANGE bounds
    // to draw from (symmetric for Transpose/Rotation, a ceiling for Length,
    // since Length has no negative/symmetric meaning) rather than literal
    // amounts - see each resolveRandom*'s own doc comment.
    const int resolvedTransposeSemitones = transposeRandom
        ? odly::resolveRandomTransposeSemitones (instanceSeed, phraseCounter, transposeSemitones)
        : transposeSemitones;
    const int resolvedRotationSteps = rotationRandom
        ? odly::resolveRandomRotationSteps (instanceSeed, phraseCounter, rotationSteps)
        : rotationSteps;
    const float resolvedLengthPercent = lengthRandom
        ? odly::resolveRandomLengthPercent (instanceSeed, phraseCounter, lengthPercent)
        : lengthPercent;
    // Quantized restricts Stretch to a fixed vocabulary of notation-friendly
    // ratios (see Docs SS16). Combined with Random, the draw happens
    // directly from that vocabulary (odly::resolveRandomQuantizedStretchPercent)
    // rather than drawing a free value and snapping it afterward - narrows
    // the choice instead of adding an escape hatch on top of one.
    const float resolvedStretchPercent = [&]
    {
        if (stretchRandom && stretchQuantized)
            return odly::resolveRandomQuantizedStretchPercent (instanceSeed, phraseCounter, stretchPercent);
        if (stretchRandom)
            return odly::resolveRandomStretchPercent (instanceSeed, phraseCounter, stretchPercent);
        if (stretchQuantized)
            return odly::snapToQuantizedStretch (stretchPercent);
        return stretchPercent;
    }();
    lastResolvedStretchPercentUi.store (resolvedStretchPercent);
    const float resolvedIntervalScalePercent = intervalRandom
        ? odly::resolveRandomIntervalPercent (instanceSeed, phraseCounter, intervalScalePercent)
        : intervalScalePercent;

    // Built ONCE here, not re-derived at fire time - see odly::Phrase's own
    // doc comment for why bundling a phrase's notes into one block's
    // emission (the old approach) was a real bug.
    phrase.outputNotes = odly::buildOutputNotes (phrase, resolvedTransposeSemitones,
                                                 resolvedRotationSteps, resolvedLengthPercent,
                                                 resolvedStretchPercent, resolvedIntervalScalePercent);
}

void OrchDelayAudioProcessor::scheduleClosedPhrase (odly::Phrase closed, int transposeSemitones)
{
    const float minimumInterestPercent = minimumInterestParameter != nullptr
        ? juce::jlimit (0.0f, 100.0f, minimumInterestParameter->load()) : 0.0f;
    const float interestPercent = odly::computePhraseInterest (closed.notes) * 100.0f;
    lastPhraseInterestUi.store (interestPercent);

    if (interestPercent < minimumInterestPercent)
    {
        totalPhrasesSkippedQualityUi.fetch_add (1);
        return;
    }

    // Multi-motive memory bank (see Docs SS21): occasionally echo an OLDER
    // captured phrase instead of this one. THIS closure's own timing
    // (scheduledFirePpq, already fixed by closePhrase before this function
    // ever ran) is never touched - only the musical CONTENT can be swapped.
    const int instanceSeed = instanceSeedParameter != nullptr
        ? juce::jlimit (0, 127, juce::roundToInt (instanceSeedParameter->load())) : 0;
    const float callbackProbabilityPercent = callbackProbabilityParameter != nullptr
        ? juce::jlimit (0.0f, 100.0f, callbackProbabilityParameter->load()) : 0.0f;

    // Capture Bank / Active Bank (see Docs SS25): callbacks always read from
    // whichever bank Active Bank points to, but what was ACTUALLY played is
    // always remembered into whichever bank Capture Bank points to - these
    // are deliberately independent selectors, not one shared knob.
    const int captureBankIndex = captureBankParameter != nullptr
        ? juce::jlimit (0, kCaptureBankChoiceCount - 1, juce::roundToInt (captureBankParameter->load())) : 0;
    const int activeBankIndex = activeBankParameter != nullptr
        ? juce::jlimit (0, kPhraseMemoryBankCount - 1, juce::roundToInt (activeBankParameter->load())) : 0;
    auto& readBank = phraseMemoryBanks[static_cast<size_t> (activeBankIndex)];
    auto& writeBank = phraseMemoryBanks[static_cast<size_t> (captureBankIndex)];

    // Recency Bias (see Docs SS26): 0 = today's flat uniform pick, unchanged;
    // above 0 biases the pool draw toward more recently-captured entries.
    const float recencyBias = recencyBiasParameter != nullptr
        ? juce::jlimit (0.0f, 1.0f, recencyBiasParameter->load() / 100.0f) : 0.0f;

    const auto decision = odly::resolveMemoryCallback (instanceSeed, phraseCounter, callbackProbabilityPercent,
                                                        static_cast<int> (readBank.size()), recencyBias);

    odly::Phrase toSchedule = closed;
    if (decision.useCallback && decision.poolIndex >= 0
        && decision.poolIndex < static_cast<int> (readBank.size()))
    {
        const auto& memory = readBank[static_cast<size_t> (decision.poolIndex)];
        toSchedule.notes = memory.notes;
        toSchedule.phraseStartPpq = memory.phraseStartPpq;
        toSchedule.phraseEndPpq = memory.phraseEndPpq;
        totalMemoryCallbacksUi.fetch_add (1);
    }

    // Remember what was ACTUALLY played (never a callback substitution
    // itself) for future callbacks to reach back to - added AFTER the
    // decision above, so a phrase can never call back to itself.
    odly::MemoryEntry entry;
    entry.notes = closed.notes;
    entry.phraseStartPpq = closed.phraseStartPpq;
    entry.phraseEndPpq = closed.phraseEndPpq;
    writeBank.push_back (entry);
    if (writeBank.size() > static_cast<size_t> (kMaxPhraseMemorySize))
        writeBank.erase (writeBank.begin());

    // Cross-instance broadcast plumbing (see OrchDelayLink / Docs SS27):
    // bump the generation counter unconditionally (cheap, harmless when no
    // one is listening) and try to snapshot the entry non-blocking - see
    // this member's own doc comment in the header for why try_lock, never a
    // blocking lock, from the audio thread.
    capturedGenerationUi.fetch_add (1);
    if (lastCapturedMutex.try_lock())
    {
        lastCapturedEntry = entry;
        lastCapturedMutex.unlock();
    }

    resolveAndScheduleTransform (toSchedule, transposeSemitones);
    pendingPhrases.push_back (toSchedule);
}

void OrchDelayAudioProcessor::checkAutonomousFire (double blockEndPpq,
                                                   double beatsPerBarNow, int transposeSemitones)
{
    const int autonomousFireBars = autonomousFireBarsParameter != nullptr
        ? juce::jlimit (0, 16, juce::roundToInt (autonomousFireBarsParameter->load())) : 0;

    // Autonomous Fire always draws from Active Bank (see Docs SS25) - the
    // same bank Callback Probability reads from, so switching Active Bank
    // redirects both mechanisms together.
    const int activeBankIndex = activeBankParameter != nullptr
        ? juce::jlimit (0, kPhraseMemoryBankCount - 1, juce::roundToInt (activeBankParameter->load())) : 0;
    auto& activeBank = phraseMemoryBanks[static_cast<size_t> (activeBankIndex)];

    if (autonomousFireBars <= 0 || activeBank.empty())
    {
        autonomousFireArmed = false;   // off, or nothing to fire yet - re-arm fresh whenever this becomes usable
        return;
    }

    const double intervalPpq = static_cast<double> (autonomousFireBars) * beatsPerBarNow;

    if (! autonomousFireArmed)
    {
        // First usable block since being (re-)armed - the clock starts
        // ticking from here, not from some earlier moment before Autonomous
        // Fire had anything to draw from.
        nextAutonomousFirePpq = blockEndPpq + intervalPpq;
        autonomousFireArmed = true;
        return;
    }

    if (blockEndPpq < nextAutonomousFirePpq)
        return;   // not due yet

    // The precise, grid-locked ppq this tick was actually due at - captured
    // BEFORE re-arming below. See Docs SS28: using this (not blockStartPpq)
    // as the fire anchor is what gives Autonomous Fire the same sample-
    // accurate precision every other note in this plugin already has,
    // instead of always landing at exactly sample 0 of whatever block
    // happened to notice it was due.
    const double firePpq = nextAutonomousFirePpq;

    const int instanceSeed = instanceSeedParameter != nullptr
        ? juce::jlimit (0, 127, juce::roundToInt (instanceSeedParameter->load())) : 0;
    const float recencyBias = recencyBiasParameter != nullptr
        ? juce::jlimit (0.0f, 1.0f, recencyBiasParameter->load() / 100.0f) : 0.0f;
    const int poolIndex = odly::resolveAutonomousFireIndex (instanceSeed, autonomousFireCounter,
                                                             static_cast<int> (activeBank.size()), recencyBias);
    ++autonomousFireCounter;

    // Re-arm from THIS cycle's own ideal due time, never from blockEndPpq -
    // keeps the cadence perfectly grid-locked instead of silently drifting
    // later every cycle by however much this block happened to overshoot
    // (see Docs SS28 - odly::resolveNextAutonomousFirePpq's own doc comment
    // has the full story). Done regardless of whether poolIndex somehow came
    // back invalid (shouldn't happen given the empty-pool guard above, but
    // never leave the clock silently stalled).
    nextAutonomousFirePpq = odly::resolveNextAutonomousFirePpq (firePpq, intervalPpq, blockEndPpq);

    if (poolIndex < 0 || poolIndex >= static_cast<int> (activeBank.size()))
        return;

    const auto& memory = activeBank[static_cast<size_t> (poolIndex)];
    odly::Phrase autoPhrase;
    autoPhrase.notes = memory.notes;
    autoPhrase.phraseStartPpq = memory.phraseStartPpq;
    autoPhrase.phraseEndPpq = memory.phraseEndPpq;
    autoPhrase.closed = true;
    // The precise due moment, not "whichever block noticed" - see this
    // function's own doc comment above `firePpq`.
    autoPhrase.scheduledFirePpq = firePpq;

    ++phraseCounter;   // shared with real closures - see resolveAndScheduleTransform's own use of it
    totalAutonomousFiresUi.fetch_add (1);
    resolveAndScheduleTransform (autoPhrase, transposeSemitones);
    pendingPhrases.push_back (autoPhrase);
}

void OrchDelayAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages)
{
    buffer.clear();
    const int numSamples = buffer.getNumSamples();

    // "Clear Bank" was pressed on the message thread since the last block -
    // perform the actual clear here, on the audio thread, against whichever
    // bank Capture Bank is set to RIGHT NOW (see requestClearCaptureBank's
    // own doc comment for why this can't just happen directly from the UI).
    if (clearCaptureBankRequested.exchange (false))
    {
        const int captureBankIndex = captureBankParameter != nullptr
            ? juce::jlimit (0, kCaptureBankChoiceCount - 1, juce::roundToInt (captureBankParameter->load())) : 0;
        phraseMemoryBanks[static_cast<size_t> (captureBankIndex)].clear();
    }

    // "Clear Remote" was pressed - see requestClearRemoteBank's own doc
    // comment for why Remote always gets its own trigger, independent of
    // Capture Bank's own selection.
    if (clearRemoteBankRequested.exchange (false))
        phraseMemoryBanks[static_cast<size_t> (kRemoteBankIndex)].clear();

    // Cross-instance phrase broadcast (see OrchDelayLink / Docs SS27): fold
    // any phrases OrchDelayLink's own connection thread has queued up since
    // the last block into the Remote bank - try_lock, never block real-time
    // processing for IPC's sake (see remoteInboxMutex's own doc comment).
    if (remoteInboxMutex.try_lock())
    {
        if (! remoteInboxPending.empty())
        {
            auto& remoteBank = phraseMemoryBanks[static_cast<size_t> (kRemoteBankIndex)];
            for (auto& entry : remoteInboxPending)
            {
                // Never trust another instance's own seq numbering - see
                // odly::memoryEntryFromVar's own doc comment for why.
                for (auto& note : entry.notes)
                    note.seq = nextNoteSeq++;

                remoteBank.push_back (std::move (entry));
                if (remoteBank.size() > static_cast<size_t> (kMaxPhraseMemorySize))
                    remoteBank.erase (remoteBank.begin());
                totalRemoteReceivedUi.fetch_add (1);
            }
            remoteInboxPending.clear();
        }
        remoteInboxMutex.unlock();
    }

    bool playing = false;
    bool havePpq = false;
    double bpm = 120.0;
    double blockStartPpq = lastBlockEndPpq;
    int tsNumerator = 4, tsDenominator = 4;

    if (auto* transport = getPlayHead())
    {
        if (const auto pos = transport->getPosition())
        {
            playing = pos->getIsPlaying();

            if (const auto hostBpm = pos->getBpm(); hostBpm && *hostBpm > 0.0)
                bpm = *hostBpm;

            if (const auto ppq = pos->getPpqPosition())
            {
                blockStartPpq = *ppq;
                havePpq = true;
            }

            if (const auto ts = pos->getTimeSignature(); ts && ts->numerator > 0 && ts->denominator > 0)
            {
                tsNumerator = ts->numerator;
                tsDenominator = ts->denominator;
            }
        }
    }

    haveTransportUi.store (havePpq);

    // No host transport at all (e.g. Standalone with no clock) - there is no
    // sound basis for "N bars later", so stay fully inactive and pass MIDI
    // straight through untouched rather than guessing bar position from
    // sample counts (see Docs SS3).
    if (! havePpq)
    {
        wasPlaying = playing;
        return;
    }

    const double ppqPerSample = sampleRate > 0.0 ? (bpm / 60.0) / sampleRate : 0.0;
    // No time passes on the timeline while stopped - extrapolating forward
    // by numSamples*ppqPerSample regardless of `playing` would make
    // lastBlockEndPpq drift further ahead of the actual (frozen) transport
    // position with every stopped block, which would then make an ordinary
    // resume-from-where-you-paused look like a backward jump below.
    const double blockEndPpq = playing ? (blockStartPpq + numSamples * ppqPerSample) : blockStartPpq;
    const double beatsPerBarNow = odly::beatsPerBar (tsNumerator, tsDenominator);

    const bool bypass = bypassParameter != nullptr && bypassParameter->load() >= 0.5f;

    juce::MidiBuffer output;

    if (bypass)
    {
        drainAndSilence (output, 0);
        pendingPhrases.clear();
        openPhrase = odly::Phrase {};

        for (const auto metadata : midiMessages)
            output.addEvent (metadata.getMessage(), metadata.samplePosition);

        midiMessages.swapWith (output);
        wasPlaying = playing;
        lastBlockEndPpq = blockEndPpq;
        haveLastBlockEnd = true;
        pendingPhraseCountUi.store (0);
        return;
    }

    const bool stoppedPlaying = ! playing && wasPlaying;
    // A genuine backward jump - a loop point during continuous playback, OR
    // the playhead relocated backward while stopped and then resumed - not
    // an ordinary "resume from right where you paused" transition.
    // Deliberately not gated on wasPlaying: blockEndPpq's own not-playing
    // branch just above keeps lastBlockEndPpq frozen while stopped instead
    // of drifting forward, so this stays accurate across a stop/resume too,
    // not just within continuous playback.
    const bool rewound = haveLastBlockEnd && blockStartPpq < lastBlockEndPpq - kRewindBeatsGuard;

    if (stoppedPlaying)
    {
        totalStopEventsUi.fetch_add (1);

        // Do NOT discard pendingPhrases here anymore. The original v1
        // design (Docs SS2) discarded anything mid-hold at stop, reasoning
        // that firing "now" would defeat the device's purpose - but closing
        // and SCHEDULING (not firing immediately) never needed that
        // sacrifice, and unconditionally clearing here turned out actively
        // harmful: if the host ever reports isPlaying() flipping true/false
        // more than once across what is really one user stop gesture (seen
        // live - stop events counted higher than actual stop presses), each
        // extra stoppedPlaying transition would silently wipe out the very
        // phrase the FIRST one had just closed and scheduled a moment
        // earlier, before it ever got the chance to fire. Nothing here
        // needs pendingPhrases touched at all - only the still-OPEN
        // (never-yet-closed) phrase does, below.

        // The still-OPEN phrase reflects a take that just finished -
        // stopping shortly after playing is the natural way a player
        // signals "that phrase is done," not an abandoned fragment. Close
        // and SCHEDULE it (does not fire it immediately - the normal fire
        // loop below still governs when it actually sounds): without this,
        // a real ~1-beat rest never gets the chance to elapse on its own
        // (checkPhraseTimeout only runs while playing), so the last phrase
        // played would silently vanish on every stop. On a repeated/glitchy
        // stoppedPlaying transition, openPhrase is already empty by then, so
        // this is naturally a no-op the second time - nothing to guard.
        // Hold Bars=0 pauses capturing entirely (see Docs SS17) - a
        // still-open phrase from before the pause is left exactly as it
        // was, neither closed nor discarded, so it resumes normally once
        // Hold Bars is raised again rather than being force-closed here.
        if (! openPhrase.notes.empty())
        {
            const int baseHoldBarsAtStop = holdBarsParameter != nullptr
                ? juce::jlimit (0, 16, juce::roundToInt (holdBarsParameter->load())) : 4;

            if (baseHoldBarsAtStop > 0)
            {
                const bool holdBarsRandomAtStop = holdBarsRandomParameter != nullptr
                    && holdBarsRandomParameter->load() >= 0.5f;
                const int instanceSeedAtStop = instanceSeedParameter != nullptr
                    ? juce::jlimit (0, 127, juce::roundToInt (instanceSeedParameter->load())) : 0;
                const int holdBarsAtStop = holdBarsRandomAtStop
                    ? odly::resolveRandomHoldBars (instanceSeedAtStop, phraseCounter, baseHoldBarsAtStop)
                    : baseHoldBarsAtStop;
                const int transposeSemitonesAtStop = transposeSemitonesParameter != nullptr
                    ? juce::jlimit (-48, 48, juce::roundToInt (transposeSemitonesParameter->load())) : 12;
                odly::closePhrase (openPhrase, holdBarsAtStop, beatsPerBarNow, blockStartPpq);
                ++phraseCounter;
                totalPhrasesClosedUi.fetch_add (1);
                lastScheduledFirePpqUi.store (openPhrase.scheduledFirePpq);
                scheduleClosedPhrase (openPhrase, transposeSemitonesAtStop);
                openPhrase = odly::Phrase {};
            }
        }

        drainAndSilence (output, 0);
    }

    // Only act on a backward jump while actually PLAYING through it - never
    // while stopped. Bitwig's own Stop button returns the playhead to the
    // play-start position by default (unlike most DAWs, which pause in
    // place); without this `playing` gate, that auto-return registers as a
    // backward jump on the very same stop that just closed-and-scheduled
    // the open phrase (see `stoppedPlaying` above), purging it again
    // immediately - a phrase would close, get scheduled, and vanish within
    // one single Stop press, with nothing ever reaching pendingPhrases by
    // the time the UI could show it. A relocation that happens purely while
    // stopped needs no purge at all - nothing depends on ppq continuity
    // until playback actually resumes, and blockEndPpq's own not-playing
    // branch keeps lastBlockEndPpq pinned at the true frozen position
    // throughout, so a genuine resume-from-an-earlier-point is still caught
    // correctly the moment `playing` goes true again.
    if (rewound)
        totalRewindDetectedUi.fetch_add (1);

    if (rewound && playing)
    {
        totalRewindActedUi.fetch_add (1);

        // Purge outright rather than attempting to re-map ppq across the
        // discontinuity - simple and correct (Docs SS2).
        openPhrase = odly::Phrase {};
        pendingPhrases.clear();
        drainAndSilence (output, 0);

        // Disarm Autonomous Fire's own clock - a genuine backward jump
        // means "now" moved, so the next check should re-arm fresh from
        // wherever the timeline actually is, rather than firing (or
        // staying stale-silent) against a schedule computed before the
        // jump - see Docs SS24.
        autonomousFireArmed = false;
    }

    if (playing)
    {
        const int baseHoldBars = holdBarsParameter != nullptr
            ? juce::jlimit (0, 16, juce::roundToInt (holdBarsParameter->load())) : 4;
        const double phraseGapBeats = phraseGapBeatsParameter != nullptr
            ? juce::jlimit (0.25, 8.0, static_cast<double> (phraseGapBeatsParameter->load())) : 1.0;
        const int transposeSemitones = transposeSemitonesParameter != nullptr
            ? juce::jlimit (-48, 48, juce::roundToInt (transposeSemitonesParameter->load())) : 12;

        // Hold Bars=0 pauses capturing entirely (see Docs SS17) - so the
        // parrot can be given a rest without a hard Bypass reset. Incoming
        // notes are still silently absorbed (never passed through, matching
        // the existing swallow-only design, Docs SS3.2) - they're just
        // never fed into a phrase at all while paused. Anything already
        // pending/mid-hold from before the pause is untouched, since only
        // the fire/note-off loops below (unconditional) touch it.
        if (baseHoldBars > 0)
        {
            const bool holdBarsRandom = holdBarsRandomParameter != nullptr
                && holdBarsRandomParameter->load() >= 0.5f;
            const int instanceSeedForHold = instanceSeedParameter != nullptr
                ? juce::jlimit (0, 127, juce::roundToInt (instanceSeedParameter->load())) : 0;
            const int holdBars = holdBarsRandom
                ? odly::resolveRandomHoldBars (instanceSeedForHold, phraseCounter, baseHoldBars)
                : baseHoldBars;
            const int captureMode = captureModeParameter != nullptr
                ? juce::jlimit (0, 2, juce::roundToInt (captureModeParameter->load())) : odly::kCaptureReplace;

            // --- capture incoming note-on/note-off events into phrases ----
            for (const auto metadata : midiMessages)
            {
                const auto msg = metadata.getMessage();
                if (! (msg.isNoteOn() || msg.isNoteOff()))
                    continue;   // everything else is dropped - OrchDelay only ever deals in notes

                odly::RawMidiEvent event;
                event.isNoteOn = msg.isNoteOn();
                event.channel = msg.getChannel();
                event.pitch = msg.getNoteNumber();
                event.velocity = static_cast<int> (msg.getVelocity() * 127.0f);
                event.ppq = blockStartPpq + metadata.samplePosition * ppqPerSample;

                if (event.isNoteOn)
                    totalNotesCapturedUi.fetch_add (1);

                auto result = odly::captureEvent (event, phraseGapBeats, holdBars, beatsPerBarNow,
                                                  nextNoteSeq, nextPhraseId, openPhrase);

                if (result.phraseClosed && ! result.closedPhrase.notes.empty())
                {
                    ++phraseCounter;
                    totalPhrasesClosedUi.fetch_add (1);
                    lastScheduledFirePpqUi.store (result.closedPhrase.scheduledFirePpq);
                    scheduleClosedPhrase (result.closedPhrase, transposeSemitones);
                }

                // Capture Mode (see Docs SS22): the live note is ALWAYS
                // captured into the buffer above regardless of mode - this
                // only decides whether it ALSO sounds immediately.
                // kCaptureReplace (default, original SS3.2 behavior): never
                // passes through, only the delayed echo ever sounds.
                // kCaptureOverlay: always passes through, alongside any echo.
                // kCaptureDuck: passes through only OUTSIDE the currently-
                // sounding echo's own pitch range - a note-OFF always
                // passes through if its own note-on did (tracked by
                // passthroughHeld), regardless of what the range looks like
                // by the time the note-off arrives, so nothing ever gets
                // stuck sounding downstream.
                const int passthroughChannel = juce::jlimit (1, 16, event.channel);
                const int passthroughPitch = juce::jlimit (0, 127, event.pitch);

                if (event.isNoteOn)
                {
                    const bool passThrough = captureMode == odly::kCaptureOverlay
                        || (captureMode == odly::kCaptureDuck
                            && odly::isOutsideActiveRange (activeFiredNotes, passthroughPitch));

                    if (passThrough)
                    {
                        output.addEvent (msg, metadata.samplePosition);
                        passthroughHeld[static_cast<size_t> (passthroughChannel)]
                                       [static_cast<size_t> (passthroughPitch)] = true;
                    }
                }
                else if (passthroughHeld[static_cast<size_t> (passthroughChannel)]
                                        [static_cast<size_t> (passthroughPitch)])
                {
                    output.addEvent (msg, metadata.samplePosition);
                    passthroughHeld[static_cast<size_t> (passthroughChannel)]
                                   [static_cast<size_t> (passthroughPitch)] = false;
                }
            }

            // --- proactively close a finished phrase even with no new note ----
            // Without this, a phrase whose last note is never followed by
            // anything (a clip that ends, or the last take before the player
            // stops) sits open forever and gets silently discarded on stop
            // instead of firing back - see odly::checkPhraseTimeout's own doc
            // comment.
            if (auto timeoutResult = odly::checkPhraseTimeout (blockEndPpq, phraseGapBeats, holdBars,
                                                                beatsPerBarNow, openPhrase);
                timeoutResult.phraseClosed && ! timeoutResult.closedPhrase.notes.empty())
            {
                ++phraseCounter;
                totalPhrasesClosedUi.fetch_add (1);
                lastScheduledFirePpqUi.store (timeoutResult.closedPhrase.scheduledFirePpq);
                scheduleClosedPhrase (timeoutResult.closedPhrase, transposeSemitones);
            }
        }

        // Autonomous Fire (see Docs SS24) - deliberately OUTSIDE the
        // baseHoldBars>0 gate above: it fires from the EXISTING memory
        // pool, not from newly-captured material, so it stays independent
        // of whether the device is currently "listening" for new phrases.
        checkAutonomousFire (blockEndPpq, beatsPerBarNow, transposeSemitones);

        furthestBlockPpqUi.store (blockEndPpq);

        // --- fire each note of each pending phrase INDEPENDENTLY ----------
        // Each note fires the first block its OWN outputOnsetPpq arrives -
        // never all of a phrase's notes bundled into whichever block first
        // notices the phrase overall is due (a real bug: that collapsed a
        // multi-note phrase's own rhythm into a simultaneous cluster, since
        // every note's onset got clamped into that one block's narrow
        // sample range). outputNotes was already built once, at schedule
        // time, by resolveAndScheduleTransform - nothing here re-derives
        // the transform. Not an exact [blockStartPpq, blockEndPpq) window
        // match either, for the same reason as the note-off loop below: a
        // slightly-overdue note still fires (clamped into this block) at
        // worst a few ms late, rather than silently falling through a gap
        // between two blocks' own windows and never firing at all.
        // Overlap Mode (see Docs SS17): governs whether a phrase's own
        // FIRST note is allowed to start while an earlier phrase's own
        // envelope (from ITS first note to ITS last, including its own
        // internal rests - `busyUntilPpq`, see Docs SS30 and that member's
        // own doc comment in the header) hasn't finished yet. Deliberately
        // NOT "any note audibly sounding this instant" - an earlier version
        // checked activeFiredNotes.empty() instead, which read as "free"
        // during any rest inside an otherwise still-in-progress phrase,
        // letting an unrelated Autonomous-Fire-drawn phrase start in the
        // gap: genuine cross-phrase overlap despite Wait being active, from
        // otherwise perfectly monophonic source material. Checked fresh
        // per-phrase below, never precomputed once - that's what makes
        // "only the oldest queued answer releases per busy period" fall out
        // naturally: the moment one phrase is released, busyUntilPpq
        // extends immediately, so the NEXT phrase checked in this same pass
        // correctly sees the device as busy again. Once a phrase has
        // started, its own remaining notes are NEVER re-gated against busy
        // state - only inter-phrase collisions are managed, never a single
        // answer's own internal texture (e.g. a Stretched phrase's own
        // notes overlapping each other is left alone).
        const int overlapMode = overlapModeParameter != nullptr
            ? juce::jlimit (0, 2, juce::roundToInt (overlapModeParameter->load())) : 0;

        for (auto& phrase : pendingPhrases)
        {
            if (phrase.fired || ! phrase.closed)
                continue;

            const bool started = std::any_of (phrase.outputNotes.begin(), phrase.outputNotes.end(),
                                              [] (const odly::ScheduledNote& n) { return n.emitted; });

            const bool dueThisBlock = ! started && ! phrase.outputNotes.empty()
                && phrase.outputNotes.front().outputOnsetPpq < blockEndPpq;

            if (dueThisBlock && overlapMode != odly::kOverlapOverlap)
            {
                if (busyUntilPpq >= blockEndPpq)
                {
                    if (overlapMode == odly::kOverlapSkip)
                    {
                        phrase.fired = true;   // discard entirely - never sounds
                        totalPhrasesSkippedBusyUi.fetch_add (1);
                    }
                    // Wait: leave it pending untouched, re-checked next
                    // block - do NOT fall through to the per-note loop
                    // below, which would fire its already-overdue notes now.
                    continue;
                }

                // Not busy - release it. If it had been waiting (overdue),
                // shift its whole remaining schedule to start right now,
                // preserving its own internal rhythm rather than firing
                // every already-overdue note in one clump the instant the
                // coast clears. Anchor to the precise ppq the earlier
                // phrase's own envelope actually ends (busyUntilPpq) rather
                // than blockStartPpq - see Docs SS29: always rounding up to
                // whichever block first noticed added up to a whole block
                // of pure, one-directional lateness on every Wait-release,
                // compounding across a take (each late release also delays
                // when the next queued phrase can release).
                const double releasePpq = juce::jmax (blockStartPpq, busyUntilPpq);
                const double shift = releasePpq - phrase.outputNotes.front().outputOnsetPpq;
                if (shift > 0.0)
                    odly::shiftOutputNotes (phrase, shift);
            }

            if (dueThisBlock)
            {
                // About to start this block (Overlap Mode phrases reach
                // here too, ungated). Claim exclusivity for this phrase's
                // own FULL envelope - first note to last, spanning its own
                // internal rests - so a later Wait-gated phrase can't sneak
                // into a gap between two of THIS phrase's own notes. Uses
                // outputNotes as they stand right now (post-shift, if any
                // was just applied above).
                for (const auto& n : phrase.outputNotes)
                    busyUntilPpq = juce::jmax (busyUntilPpq, n.outputOffPpq);
            }

            bool anyStillPending = false;

            for (auto& note : phrase.outputNotes)
            {
                if (note.emitted)
                    continue;
                if (note.outputOnsetPpq >= blockEndPpq)
                {
                    anyStillPending = true;
                    continue;
                }

                const int onSample = juce::jlimit (0, juce::jmax (0, numSamples - 1),
                                                   juce::roundToInt ((note.outputOnsetPpq - blockStartPpq) / ppqPerSample));

                output.addEvent (juce::MidiMessage::noteOn (note.channel, note.pitch,
                                                             static_cast<juce::uint8> (juce::jlimit (1, 127, note.velocity))),
                                 onSample);

                odly::ActiveFiredNote active;
                active.channel = note.channel;
                active.pitch = note.pitch;
                active.seq = note.seq;
                active.noteOffPpq = note.outputOffPpq;
                activeFiredNotes.push_back (active);

                note.emitted = true;
            }

            if (! anyStillPending && ! phrase.outputNotes.empty())
            {
                phrase.fired = true;
                totalPhrasesFiredUi.fetch_add (1);
            }
        }

        pendingPhrases.erase (std::remove_if (pendingPhrases.begin(), pendingPhrases.end(),
                                              [] (const odly::Phrase& p) { return p.fired; }),
                              pendingPhrases.end());

        // --- emit note-offs for anything whose scheduled off has arrived --
        // Same widened-condition reasoning as the fire loop above: a
        // due-or-overdue note-off (noteOffPpq < blockEndPpq) still fires
        // now, clamped into this block, rather than risking a stuck note if
        // its exact target ppq had fallen through a gap between two blocks'
        // own windows. Purely per-note MIDI output bookkeeping now -
        // Wait/Skip busy-gating reads busyUntilPpq instead (Docs SS30), not
        // this table, so this loop's position within the block no longer
        // matters for gating correctness.
        for (auto it = activeFiredNotes.begin(); it != activeFiredNotes.end(); )
        {
            if (it->noteOffPpq < blockEndPpq)
            {
                const int offSample = juce::jlimit (0, juce::jmax (0, numSamples - 1),
                                                    juce::roundToInt ((it->noteOffPpq - blockStartPpq) / ppqPerSample));
                output.addEvent (juce::MidiMessage::noteOff (it->channel, it->pitch), offSample);
                it = activeFiredNotes.erase (it);
            }
            else
            {
                ++it;
            }
        }
    }

    midiMessages.swapWith (output);

    wasPlaying = playing;
    lastBlockEndPpq = blockEndPpq;
    haveLastBlockEnd = true;
    pendingPhraseCountUi.store (static_cast<int> (pendingPhrases.size()));
}

juce::AudioProcessorEditor* OrchDelayAudioProcessor::createEditor()
{
    return new OrchDelayAudioProcessorEditor (*this);
}

bool OrchDelayAudioProcessor::hasEditor() const
{
    return true;
}

const juce::String OrchDelayAudioProcessor::getName() const
{
    return JucePlugin_Name;
}

bool OrchDelayAudioProcessor::acceptsMidi() const
{
    return true;
}

bool OrchDelayAudioProcessor::producesMidi() const
{
    return true;
}

bool OrchDelayAudioProcessor::isMidiEffect() const
{
    return true;
}

double OrchDelayAudioProcessor::getTailLengthSeconds() const
{
    return 0.0;
}

int OrchDelayAudioProcessor::getNumPrograms()
{
    return 1;
}

int OrchDelayAudioProcessor::getCurrentProgram()
{
    return 0;
}

void OrchDelayAudioProcessor::setCurrentProgram (int index)
{
    juce::ignoreUnused (index);
}

const juce::String OrchDelayAudioProcessor::getProgramName (int index)
{
    juce::ignoreUnused (index);
    return {};
}

void OrchDelayAudioProcessor::changeProgramName (int index, const juce::String& newName)
{
    juce::ignoreUnused (index, newName);
}

void OrchDelayAudioProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    if (auto state = parameters.copyState(); state.isValid())
    {
        std::unique_ptr<juce::XmlElement> xml (state.createXml());

        if (xml != nullptr)
        {
            // Instance Label (Docs SS31) isn't an APVTS parameter (see
            // getInstanceLabelForUi's own doc comment) - piggyback it as a
            // plain XML attribute on the same root element instead.
            xml->setAttribute ("instanceLabel", getInstanceLabelForUi());
            copyXmlToBinary (*xml, destData);
        }
    }
}

void OrchDelayAudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    std::unique_ptr<juce::XmlElement> xml (getXmlFromBinary (data, sizeInBytes));

    if (xml != nullptr && xml->hasTagName (parameters.state.getType()))
    {
        setInstanceLabel (xml->getStringAttribute ("instanceLabel"));
        parameters.replaceState (juce::ValueTree::fromXml (*xml));
    }

    openPhrase = odly::Phrase {};
    pendingPhrases.clear();
    activeFiredNotes.clear();
    busyUntilPpq = -1.0;
    wasPlaying = false;
    haveLastBlockEnd = false;
}

juce::AudioProcessorValueTreeState& OrchDelayAudioProcessor::getParameters()
{
    return parameters;
}

void OrchDelayAudioProcessor::randomizeInstanceSeed()
{
    // Writes through the normal parameter path - undoable, saved in state,
    // never a bare non-parameter side value (see this method's own doc
    // comment in the header). Never called automatically on load/reload.
    if (auto* p = parameters.getParameter ("instanceSeed"))
        p->setValueNotifyingHost (juce::Random::getSystemRandom().nextFloat());
}

void OrchDelayAudioProcessor::requestClearCaptureBank()
{
    // Just raises a flag - see this method's own doc comment in the header
    // for why the actual clear has to happen on the audio thread instead.
    clearCaptureBankRequested.store (true);
}

void OrchDelayAudioProcessor::requestClearRemoteBank()
{
    clearRemoteBankRequested.store (true);
}

odly::MemoryEntry OrchDelayAudioProcessor::snapshotLastCapturedForBroadcast() const
{
    // Blocking lock is fine here - called only from OrchDelayLink's own
    // worker thread, never the audio thread (see this method's own doc
    // comment in the header).
    std::lock_guard<std::mutex> lock (lastCapturedMutex);
    return lastCapturedEntry;
}

void OrchDelayAudioProcessor::pushIncomingRemotePhrase (const odly::MemoryEntry& entry)
{
    // Blocking lock is fine here too - called only from OrchDelayLink's own
    // connection thread, never the audio thread (see this method's own doc
    // comment in the header). processBlock() drains this with a try_lock.
    std::lock_guard<std::mutex> lock (remoteInboxMutex);
    remoteInboxPending.push_back (entry);
    if (remoteInboxPending.size() > 64)   // safety cap - never grow unbounded if processBlock stalls
        remoteInboxPending.erase (remoteInboxPending.begin());
}

juce::String OrchDelayAudioProcessor::getInstanceLabelForUi() const
{
    std::lock_guard<std::mutex> lock (instanceLabelMutex);
    return instanceLabel;
}

void OrchDelayAudioProcessor::setInstanceLabel (const juce::String& label)
{
    std::lock_guard<std::mutex> lock (instanceLabelMutex);
    instanceLabel = label;
}

juce::AudioProcessorValueTreeState::ParameterLayout OrchDelayAudioProcessor::createParameterLayout()
{
    std::vector<std::unique_ptr<juce::RangedAudioParameter>> params;

    params.push_back (std::make_unique<juce::AudioParameterBool> (
        juce::ParameterID { "bypass", 1 },
        "Bypass",
        false));

    // How live input relates to a currently-sounding echo (see
    // odly::CaptureMode / Docs SS22). Reopens the original "live notes are
    // always fully swallowed" decision - kept as the default (Replace),
    // still exactly right for a clean call-and-response device, but not
    // every use wants that. The live note is ALWAYS still captured into the
    // buffer for its own future echo regardless of this setting - it only
    // decides whether it ALSO sounds immediately.
    params.push_back (std::make_unique<juce::AudioParameterChoice> (
        juce::ParameterID { "captureMode", 1 },
        "Capture Mode",
        juce::StringArray { "Replace", "Overlay", "Duck" },
        0));

    // Lets the device fire from its own memory pool on its own clock,
    // entirely independent of new incoming MIDI, once seeded with at least
    // one real captured phrase (see odly::resolveAutonomousFireIndex /
    // Docs SS24). 0 = off (default) - the device stays purely reactive,
    // today's original behavior.
    params.push_back (std::make_unique<juce::AudioParameterInt> (
        juce::ParameterID { "autonomousFireBars", 1 },
        "Autonomous Fire (bars)",
        0, 16, 0));

    // Multi-bank memory (see Docs SS25): 3 independent pools (A/B/C), like a
    // development section where material from different sections is built up
    // separately, then mixed in. Deliberately DECOUPLED from Active Bank
    // below - lets you keep playing from one bank while building up new
    // material in another via a different capture MIDI clip, then switch
    // over by changing Active Bank alone.
    params.push_back (std::make_unique<juce::AudioParameterChoice> (
        juce::ParameterID { "captureBank", 1 },
        "Capture Bank",
        juce::StringArray { "A", "B", "C" },
        0));

    // Which bank Callback Probability and Autonomous Fire draw from. Kept
    // separate from Capture Bank above on purpose - see this block's own
    // comment for why. 4th choice "Remote" (see Docs SS27) draws from phrases
    // received from OTHER instances over OrchDelayLink instead of this
    // instance's own local A/B/C material.
    params.push_back (std::make_unique<juce::AudioParameterChoice> (
        juce::ParameterID { "activeBank", 1 },
        "Active Bank",
        juce::StringArray { "A", "B", "C", "Remote" },
        0));

    // Recency Bias (see odly::resolveRecencyWeightedPoolIndex / Docs SS26):
    // 0% (default) keeps Callback Probability and Autonomous Fire's pool
    // draw exactly as flat/uniform as it always was; raising it biases both
    // toward more recently-captured entries in Active Bank instead of
    // any-age-equally, without touching which bank is in play.
    params.push_back (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { "recencyBias", 1 },
        "Recency Bias",
        juce::NormalisableRange<float> (0.0f, 100.0f, 1.0f),
        0.0f,
        juce::AudioParameterFloatAttributes()
            .withLabel ("%")
            .withStringFromValueFunction ([] (float value, int)
            {
                return juce::String (juce::roundToInt (value)) + "%";
            })
            .withValueFromStringFunction ([] (const juce::String& text)
            {
                return text.retainCharacters ("0123456789.").getFloatValue();
            })));

    // Cross-instance phrase broadcast (see OrchDelayLink / Docs SS27): lets
    // one instance's captured phrases feed another instance's Remote bank
    // directly, over a local loopback link - no MIDI cable needed between
    // tracks. Exactly ONE instance in the whole rig should have this on; it
    // becomes the shared relay hub everyone else connects to (same
    // convention OrchCapture's own Coordinator toggle and OrchMerge's own
    // Hub toggle already use in this ecosystem).
    params.push_back (std::make_unique<juce::AudioParameterBool> (
        juce::ParameterID { "linkHub", 1 },
        "Broadcast Hub",
        false));

    // 0 = off (default): this instance's own captured phrases are never
    // broadcast. Above 0, every phrase captured into Capture Bank is also
    // published on this channel number to whichever instance is the hub.
    params.push_back (std::make_unique<juce::AudioParameterInt> (
        juce::ParameterID { "broadcastChannel", 1 },
        "Broadcast Channel",
        0, 8, 0));

    // 0 = off (default): this instance never receives anything. Above 0,
    // phrases broadcast on this SAME channel number by OTHER instances (this
    // instance's own broadcasts are never relayed back to itself) land in
    // this instance's own Remote bank.
    params.push_back (std::make_unique<juce::AudioParameterInt> (
        juce::ParameterID { "listenChannel", 1 },
        "Listen Channel",
        0, 8, 0));

    // The core "how far in the future" control - the whole point of the
    // device. 0 is a dedicated "pause capturing" state (see Docs SS17) -
    // the parrot stops listening for new phrases entirely until raised back
    // to 1+, without needing a hard Bypass reset.
    params.push_back (std::make_unique<juce::AudioParameterInt> (
        juce::ParameterID { "holdBars", 1 },
        "Hold Bars",
        0, 16, 4));

    // When on, drawn per phrase from [1, Hold Bars] instead of the fixed
    // value - never 0, a random draw should never silently re-enable
    // capturing by chance if the base Hold Bars is deliberately paused (see
    // odly::resolveRandomHoldBars).
    params.push_back (std::make_unique<juce::AudioParameterBool> (
        juce::ParameterID { "holdBarsRandom", 1 },
        "Random Hold Bars",
        false));

    // Governs what happens when a due answer's own start collides with an
    // earlier answer still audibly sounding (see Docs SS17 - not every
    // instrument downstream is polyphonic, most of an orchestra isn't).
    params.push_back (std::make_unique<juce::AudioParameterChoice> (
        juce::ParameterID { "overlapMode", 1 },
        "Overlap Mode",
        juce::StringArray { "Overlap", "Wait", "Skip" },
        0));

    // Silence-gap threshold that closes a captured phrase - exposed (unlike
    // OrchPiano's own hardcoded 1.0-beat equivalent), since phrase length
    // matters far more for a responsorial device than for a reduction tool.
    params.push_back (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { "phraseGapBeats", 1 },
        "Phrase Gap (beats)",
        juce::NormalisableRange<float> (0.25f, 8.0f, 0.25f),
        1.0f));

    // Probability of proposing a non-verbatim transform per phrase (see
    // odly::proposeTransform) - default 0 = always verbatim until opted in,
    // the safest default for a device whose baseline behavior should be
    // predictable.
    params.push_back (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { "restlessness", 1 },
        "Restlessness",
        juce::NormalisableRange<float> (0.0f, 1.0f, 0.01f),
        0.0f,
        juce::AudioParameterFloatAttributes()
            .withLabel ("%")
            .withStringFromValueFunction ([] (float value, int)
            {
                return juce::String (juce::roundToInt (value * 100.0f)) + "%";
            })
            .withValueFromStringFunction ([] (const juce::String& text)
            {
                return text.retainCharacters ("0123456789.").getFloatValue() / 100.0f;
            })));

    // Manual override - any explicit choice always wins outright over the
    // restlessness proposal (see resolveAndScheduleTransform). v1.1 adds
    // Rotation/Length/M7 to the original v1 set (Transpose/Retrograde/
    // Inversion) - see Docs SS14.
    params.push_back (std::make_unique<juce::AudioParameterChoice> (
        juce::ParameterID { "manualTransform", 1 },
        "Transform",
        juce::StringArray { "Follow Restlessness", "None", "Transpose", "Retrograde", "Inversion",
                            "Rotation", "Length", "M7", "Stretch", "Interval" },
        0));

    params.push_back (std::make_unique<juce::AudioParameterInt> (
        juce::ParameterID { "transposeSemitones", 1 },
        "Transpose (semitones)",
        -48, 48, 12));

    // When on, the amount actually used per phrase is drawn deterministically
    // (Instance Seed + phrase count, reload-stable) from [-|Transpose|,
    // +|Transpose|] instead of always using the fixed Transpose value - see
    // odly::resolveRandomTransposeSemitones. Only affects the Transpose
    // transform, whether reached manually or via Follow Restlessness.
    params.push_back (std::make_unique<juce::AudioParameterBool> (
        juce::ParameterID { "transposeRandom", 1 },
        "Random Transpose",
        false));

    // Rotation amount - cyclic reassignment of which captured note's pitch
    // plays at each onset slot (see odly::applyRotation). Wraps automatically
    // to the actual phrase's own note count, so this fixed UI range is just a
    // convenient sweep, not a hard musical limit.
    params.push_back (std::make_unique<juce::AudioParameterInt> (
        juce::ParameterID { "rotationSteps", 1 },
        "Rotation (steps)",
        -16, 16, 1));

    // When on, drawn per phrase from [-|Rotation|, +|Rotation|] instead of
    // the fixed value - see odly::resolveRandomRotationSteps.
    params.push_back (std::make_unique<juce::AudioParameterBool> (
        juce::ParameterID { "rotationRandom", 1 },
        "Random Rotation",
        false));

    // How much of the captured phrase (by note count, first-to-last) actually
    // gets echoed - see odly::applyLength. 100% = the whole phrase, matching
    // every other transform's own "no shortening" baseline.
    params.push_back (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { "lengthPercent", 1 },
        "Length (%)",
        juce::NormalisableRange<float> (1.0f, 100.0f, 1.0f),
        50.0f,
        juce::AudioParameterFloatAttributes()
            .withLabel ("%")
            .withStringFromValueFunction ([] (float value, int)
            {
                return juce::String (juce::roundToInt (value)) + "%";
            })
            .withValueFromStringFunction ([] (const juce::String& text)
            {
                return text.retainCharacters ("0123456789.").getFloatValue();
            })));

    // When on, drawn per phrase from [1%, Length%] instead of the fixed
    // value - Length has no negative/symmetric meaning, so the slider
    // becomes a ceiling here, not a symmetric bound - see
    // odly::resolveRandomLengthPercent.
    params.push_back (std::make_unique<juce::AudioParameterBool> (
        juce::ParameterID { "lengthRandom", 1 },
        "Random Length",
        false));

    // Proportional time-stretch of the echoed phrase - see odly::applyStretch.
    // 100% = unchanged; not achievable as a live, per-echo, potentially-
    // randomized effect via a static Bitwig clip edit (the user's own point
    // in proposing this transform).
    params.push_back (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { "stretchPercent", 1 },
        "Stretch (%)",
        juce::NormalisableRange<float> (25.0f, 400.0f, 1.0f),
        100.0f,
        juce::AudioParameterFloatAttributes()
            .withLabel ("%")
            .withStringFromValueFunction ([] (float value, int)
            {
                return juce::String (juce::roundToInt (value)) + "%";
            })
            .withValueFromStringFunction ([] (const juce::String& text)
            {
                return text.retainCharacters ("0123456789.").getFloatValue();
            })));

    // When on, drawn per phrase from [100%, Stretch%] (whichever side of
    // 100 the slider sits on) instead of the fixed value - Stretch's
    // neutral point is 100%, not 0, so this isn't a symmetric bound like
    // Transpose/Rotation's own Random modes - see
    // odly::resolveRandomStretchPercent.
    params.push_back (std::make_unique<juce::AudioParameterBool> (
        juce::ParameterID { "stretchRandom", 1 },
        "Random Stretch",
        false));

    // When on, restricts the actual stretch ratio used to a fixed vocabulary
    // of "notation-friendly" values (see odly::quantizedStretchRatios) -
    // arbitrary in-between percentages rescale a phrase's timing off any
    // grid a notation program can render cleanly, so this snaps a fixed
    // value to the nearest legal ratio, or (combined with Random Stretch)
    // draws directly from the legal set instead of a free continuous range.
    params.push_back (std::make_unique<juce::AudioParameterBool> (
        juce::ParameterID { "stretchQuantized", 1 },
        "Quantized Stretch",
        false));

    // Scales every note's interval from the phrase's own anchor note (see
    // odly::applyIntervalScale) - 100% = unchanged, >100% widens the
    // melodic shape's leaps, <100% narrows them. Defaults to 150% (not the
    // 100% no-op) so picking "Interval" from the Transform menu is
    // immediately audible, matching Transpose/Rotation's own choice to
    // default away from silence.
    params.push_back (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { "intervalScalePercent", 1 },
        "Interval Scale (%)",
        juce::NormalisableRange<float> (0.0f, 300.0f, 1.0f),
        150.0f,
        juce::AudioParameterFloatAttributes()
            .withLabel ("%")
            .withStringFromValueFunction ([] (float value, int)
            {
                return juce::String (juce::roundToInt (value)) + "%";
            })
            .withValueFromStringFunction ([] (const juce::String& text)
            {
                return text.retainCharacters ("0123456789.").getFloatValue();
            })));

    // When on, drawn per phrase from [100%, Interval Scale%] (whichever side
    // of 100 the slider sits on) instead of the fixed value - same
    // asymmetric-bound convention as Random Stretch, since Interval Scale's
    // neutral point is also 100%, not 0 - see odly::resolveRandomIntervalPercent.
    params.push_back (std::make_unique<juce::AudioParameterBool> (
        juce::ParameterID { "intervalRandom", 1 },
        "Random Interval",
        false));

    // Biases WHICH transform Follow Restlessness picks by the phrase's own
    // features (density/pitch spread/note count) instead of a flat 1-in-8
    // chance - see odly::proposeWeightedTransform / Docs SS19. Default ON:
    // a strict improvement over the original equal-weight pick, with the
    // toggle kept for anyone who wants the older, purely-uniform behavior
    // back. Has no effect when Transform is set to an explicit manual
    // choice rather than Follow Restlessness.
    params.push_back (std::make_unique<juce::AudioParameterBool> (
        juce::ParameterID { "contentAwareWeighting", 1 },
        "Content-Aware Weighting",
        true));

    // Phrase-quality gate (see odly::computePhraseInterest / Docs SS20): a
    // captured phrase below this 0-100% "worth answering" score is closed
    // normally but never scheduled to echo at all. Default 0% = gate fully
    // disabled (every phrase always echoes, today's original behavior) -
    // unlike Content-Aware Weighting, this can actively discard material,
    // so it stays opt-in rather than defaulting on.
    params.push_back (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { "minimumInterest", 1 },
        "Minimum Interest",
        juce::NormalisableRange<float> (0.0f, 100.0f, 1.0f),
        0.0f,
        juce::AudioParameterFloatAttributes()
            .withLabel ("%")
            .withStringFromValueFunction ([] (float value, int)
            {
                return juce::String (juce::roundToInt (value)) + "%";
            })
            .withValueFromStringFunction ([] (const juce::String& text)
            {
                return text.retainCharacters ("0123456789.").getFloatValue();
            })));

    // Multi-motive memory bank (see odly::resolveMemoryCallback / Docs
    // SS21): the probability that a just-closed phrase echoes an OLDER
    // captured phrase instead of itself - the echo's own timing (Hold Bars
    // etc.) is never affected, only which musical material gets used.
    // Default 0% = feature fully disabled (today's original "only ever
    // remembers the most recent phrase" behavior).
    params.push_back (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { "callbackProbability", 1 },
        "Callback Probability",
        juce::NormalisableRange<float> (0.0f, 100.0f, 1.0f),
        0.0f,
        juce::AudioParameterFloatAttributes()
            .withLabel ("%")
            .withStringFromValueFunction ([] (float value, int)
            {
                return juce::String (juce::roundToInt (value)) + "%";
            })
            .withValueFromStringFunction ([] (const juce::String& text)
            {
                return text.retainCharacters ("0123456789.").getFloatValue();
            })));

    // This instance's own hash key for the restlessness proposal (see
    // odly::proposeTransform / fnv1aHash) - gives multiple OrchDelay
    // instances in a rig deterministic, mutually divergent behavior with no
    // shared state/IPC needed, the same property OrchGate's own response-
    // bridge hash achieves for its own instances. The editor's "Randomize"
    // button re-rolls this through the normal parameter path.
    params.push_back (std::make_unique<juce::AudioParameterInt> (
        juce::ParameterID { "instanceSeed", 1 },
        "Instance Seed",
        0, 127, 0));

    return { params.begin(), params.end() };
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new OrchDelayAudioProcessor();
}
