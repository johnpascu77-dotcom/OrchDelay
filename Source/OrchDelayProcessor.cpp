#include "OrchDelayProcessor.h"
#include "OrchDelayEditor.h"

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
    instanceSeedParameter = parameters.getRawParameterValue ("instanceSeed");
}

void OrchDelayAudioProcessor::prepareToPlay (double newSampleRate, int samplesPerBlock)
{
    juce::ignoreUnused (samplesPerBlock);
    sampleRate = newSampleRate;

    openPhrase = odly::Phrase {};
    pendingPhrases.clear();
    activeFiredNotes.clear();
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
    const int rotationSteps = rotationStepsParameter != nullptr
        ? juce::roundToInt (rotationStepsParameter->load()) : 0;
    const float lengthPercent = lengthPercentParameter != nullptr
        ? juce::jlimit (0.0f, 100.0f, lengthPercentParameter->load()) : 100.0f;
    const float stretchPercent = stretchPercentParameter != nullptr
        ? juce::jlimit (25.0f, 400.0f, stretchPercentParameter->load()) : 100.0f;

    // Choice indices: 0=Follow Restlessness, 1=None, 2=Transpose, 3=Retrograde,
    // 4=Inversion, 5=Rotation, 6=Length, 7=M7 - any explicit choice (>0)
    // always wins outright, no blending with the proposal mechanism (see
    // this header's own doc comment).
    if (manualChoice > 0)
        phrase.chosenTransform = manualChoice - 1;   // maps directly onto odly::TransformKind
    else
    {
        const auto proposal = odly::proposeTransform (instanceSeed, phraseCounter, restlessness);
        phrase.chosenTransform = proposal.applyAny ? proposal.transformKind : odly::kTransformNone;
    }

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
    const float resolvedStretchPercent = stretchRandom
        ? odly::resolveRandomStretchPercent (instanceSeed, phraseCounter, stretchPercent)
        : stretchPercent;

    // Built ONCE here, not re-derived at fire time - see odly::Phrase's own
    // doc comment for why bundling a phrase's notes into one block's
    // emission (the old approach) was a real bug.
    phrase.outputNotes = odly::buildOutputNotes (phrase, resolvedTransposeSemitones,
                                                 resolvedRotationSteps, resolvedLengthPercent,
                                                 resolvedStretchPercent);
}

void OrchDelayAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages)
{
    buffer.clear();
    const int numSamples = buffer.getNumSamples();

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
        if (! openPhrase.notes.empty())
        {
            const int holdBarsAtStop = holdBarsParameter != nullptr
                ? juce::jlimit (1, 16, juce::roundToInt (holdBarsParameter->load())) : 4;
            const int transposeSemitonesAtStop = transposeSemitonesParameter != nullptr
                ? juce::jlimit (-48, 48, juce::roundToInt (transposeSemitonesParameter->load())) : 12;
            odly::closePhrase (openPhrase, holdBarsAtStop, beatsPerBarNow, blockStartPpq);
            ++phraseCounter;
            totalPhrasesClosedUi.fetch_add (1);
            lastScheduledFirePpqUi.store (openPhrase.scheduledFirePpq);
            odly::Phrase closed = openPhrase;
            resolveAndScheduleTransform (closed, transposeSemitonesAtStop);
            pendingPhrases.push_back (closed);
        }
        openPhrase = odly::Phrase {};

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
    }

    if (playing)
    {
        const int holdBars = holdBarsParameter != nullptr
            ? juce::jlimit (1, 16, juce::roundToInt (holdBarsParameter->load())) : 4;
        const double phraseGapBeats = phraseGapBeatsParameter != nullptr
            ? juce::jlimit (0.25, 8.0, static_cast<double> (phraseGapBeatsParameter->load())) : 1.0;
        const int transposeSemitones = transposeSemitonesParameter != nullptr
            ? juce::jlimit (-48, 48, juce::roundToInt (transposeSemitonesParameter->load())) : 12;

        // --- capture incoming note-on/note-off events into phrases --------
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
                odly::Phrase closed = result.closedPhrase;
                resolveAndScheduleTransform (closed, transposeSemitones);
                pendingPhrases.push_back (closed);
            }

            // The live note-on/off is swallowed into the buffer, never
            // passed through live - see Docs SS3.2 (only the delayed,
            // possibly-transformed echo ever sounds).
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
            odly::Phrase closed = timeoutResult.closedPhrase;
            resolveAndScheduleTransform (closed, transposeSemitones);
            pendingPhrases.push_back (closed);
        }

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
        for (auto& phrase : pendingPhrases)
        {
            if (phrase.fired || ! phrase.closed)
                continue;

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
        // own windows.
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
            copyXmlToBinary (*xml, destData);
    }
}

void OrchDelayAudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    std::unique_ptr<juce::XmlElement> xml (getXmlFromBinary (data, sizeInBytes));

    if (xml != nullptr && xml->hasTagName (parameters.state.getType()))
        parameters.replaceState (juce::ValueTree::fromXml (*xml));

    openPhrase = odly::Phrase {};
    pendingPhrases.clear();
    activeFiredNotes.clear();
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

juce::AudioProcessorValueTreeState::ParameterLayout OrchDelayAudioProcessor::createParameterLayout()
{
    std::vector<std::unique_ptr<juce::RangedAudioParameter>> params;

    params.push_back (std::make_unique<juce::AudioParameterBool> (
        juce::ParameterID { "bypass", 1 },
        "Bypass",
        false));

    // The core "how far in the future" control - the whole point of the device.
    params.push_back (std::make_unique<juce::AudioParameterInt> (
        juce::ParameterID { "holdBars", 1 },
        "Hold Bars",
        1, 16, 4));

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
                            "Rotation", "Length", "M7", "Stretch" },
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
