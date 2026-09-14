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

void OrchDelayAudioProcessor::resolveAndScheduleTransform (odly::Phrase& phrase)
{
    const float restlessness = restlessnessParameter != nullptr ? restlessnessParameter->load() : 0.0f;
    const int manualChoice = manualTransformParameter != nullptr
        ? juce::roundToInt (manualTransformParameter->load()) : 0;
    const int instanceSeed = instanceSeedParameter != nullptr
        ? juce::jlimit (0, 127, juce::roundToInt (instanceSeedParameter->load())) : 0;

    // Choice indices: 0=Follow Restlessness, 1=None, 2=Transpose, 3=Retrograde,
    // 4=Inversion - any explicit choice (>0) always wins outright, no blending
    // with the proposal mechanism (see this header's own doc comment).
    if (manualChoice > 0)
    {
        phrase.chosenTransform = manualChoice - 1;   // maps directly onto odly::TransformKind
        return;
    }

    const auto proposal = odly::proposeTransform (instanceSeed, phraseCounter, restlessness);
    phrase.chosenTransform = proposal.applyAny ? proposal.transformKind : odly::kTransformNone;
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
        // A phrase already closed and mid-hold (scheduled from BEFORE this
        // stop) is DISCARDED, not force-fired - firing "now" defeats the
        // device's whole purpose (Docs SS2).
        pendingPhrases.clear();

        // But the still-OPEN (never-yet-closed) phrase reflects a take that
        // just finished - stopping shortly after playing is the natural way
        // a player signals "that phrase is done," not an abandoned
        // fragment. Close and SCHEDULE it (does not fire it immediately -
        // the normal fire loop below still governs when it actually
        // sounds), rather than discarding it outright: without this, a real
        // ~1-beat rest never gets the chance to elapse on its own
        // (checkPhraseTimeout only runs while playing), so the last phrase
        // played would silently vanish on every stop.
        if (! openPhrase.notes.empty())
        {
            const int holdBarsAtStop = holdBarsParameter != nullptr
                ? juce::jlimit (1, 16, juce::roundToInt (holdBarsParameter->load())) : 4;
            odly::closePhrase (openPhrase, holdBarsAtStop, beatsPerBarNow);
            ++phraseCounter;
            odly::Phrase closed = openPhrase;
            resolveAndScheduleTransform (closed);
            pendingPhrases.push_back (closed);
        }
        openPhrase = odly::Phrase {};

        drainAndSilence (output, 0);
    }

    if (rewound)
    {
        // Purge outright rather than attempting to re-map ppq across the
        // discontinuity - simple and correct (Docs SS2). Covers both a
        // loop/relocate during continuous playback and the playhead moved
        // backward while stopped, then resumed - an ordinary resume from
        // exactly where playback paused does NOT land here (see `rewound`'s
        // own doc comment), so a phrase closed-and-scheduled at stop
        // survives a plain stop/resume and still fires on schedule.
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

            auto result = odly::captureEvent (event, phraseGapBeats, holdBars, beatsPerBarNow,
                                              nextNoteSeq, nextPhraseId, openPhrase);

            if (result.phraseClosed && ! result.closedPhrase.notes.empty())
            {
                ++phraseCounter;
                odly::Phrase closed = result.closedPhrase;
                resolveAndScheduleTransform (closed);
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
            odly::Phrase closed = timeoutResult.closedPhrase;
            resolveAndScheduleTransform (closed);
            pendingPhrases.push_back (closed);
        }

        // --- fire any phrase whose scheduled time falls in this block -----
        for (auto& phrase : pendingPhrases)
        {
            if (phrase.fired || ! phrase.closed)
                continue;
            if (phrase.scheduledFirePpq < blockStartPpq || phrase.scheduledFirePpq >= blockEndPpq)
                continue;

            const auto transformed = odly::applyTransform (phrase.notes, phrase.chosenTransform,
                                                            phrase.phraseStartPpq, phrase.phraseEndPpq,
                                                            transposeSemitones);

            for (const auto& note : transformed)
            {
                const double outOnsetPpq = phrase.scheduledFirePpq + (note.onsetPpq - phrase.phraseStartPpq);
                const double outOffPpq = outOnsetPpq + note.durationPpq;
                const int onSample = juce::jlimit (0, juce::jmax (0, numSamples - 1),
                                                   juce::roundToInt ((outOnsetPpq - blockStartPpq) / ppqPerSample));

                output.addEvent (juce::MidiMessage::noteOn (note.channel, note.pitch,
                                                             static_cast<juce::uint8> (juce::jlimit (1, 127, note.velocity))),
                                 onSample);

                odly::ActiveFiredNote active;
                active.channel = note.channel;
                active.pitch = note.pitch;
                active.seq = note.seq;
                active.noteOffPpq = outOffPpq;
                activeFiredNotes.push_back (active);
            }

            phrase.fired = true;
        }

        pendingPhrases.erase (std::remove_if (pendingPhrases.begin(), pendingPhrases.end(),
                                              [] (const odly::Phrase& p) { return p.fired; }),
                              pendingPhrases.end());

        // --- emit note-offs for anything whose scheduled off falls here ---
        for (auto it = activeFiredNotes.begin(); it != activeFiredNotes.end(); )
        {
            if (it->noteOffPpq >= blockStartPpq && it->noteOffPpq < blockEndPpq)
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
    // restlessness proposal (see resolveAndScheduleTransform).
    params.push_back (std::make_unique<juce::AudioParameterChoice> (
        juce::ParameterID { "manualTransform", 1 },
        "Transform",
        juce::StringArray { "Follow Restlessness", "None", "Transpose", "Retrograde", "Inversion" },
        0));

    params.push_back (std::make_unique<juce::AudioParameterInt> (
        juce::ParameterID { "transposeSemitones", 1 },
        "Transpose (semitones)",
        -48, 48, 12));

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
