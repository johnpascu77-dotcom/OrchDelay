#include "OrchDelayLogic.h"

#include <algorithm>

namespace odly
{
    namespace
    {
        constexpr double kFallbackDurationBeats = 0.5;   // see closePhrase's own doc comment
    }

    double beatsPerBar (int numerator, int denominator)
    {
        if (denominator <= 0)
            return 4.0;   // degenerate host report - fall back to a bar of 4 quarter notes
        return numerator * 4.0 / static_cast<double> (denominator);
    }

    void closePhrase (Phrase& phrase, int holdBars, double beatsPerBarNow)
    {
        if (! phrase.notes.empty())
            phrase.phraseStartPpq = phrase.notes.front().onsetPpq;

        double latestEndPpq = phrase.phraseStartPpq;

        for (auto& n : phrase.notes)
        {
            if (! n.hasNoteOff)
            {
                n.durationPpq = kFallbackDurationBeats;
                n.hasNoteOff = true;   // makes a later real note-off for it read as a stray, per captureEvent's doc
            }
            latestEndPpq = juce::jmax (latestEndPpq, n.onsetPpq + n.durationPpq);
        }

        phrase.phraseEndPpq = latestEndPpq;
        phrase.scheduledFirePpq = phrase.phraseStartPpq + juce::jmax (1, holdBars) * beatsPerBarNow;
        phrase.closed = true;
    }

    CaptureResult captureEvent (const RawMidiEvent& event, double phraseGapBeats,
                                int holdBars, double beatsPerBarNow,
                                juce::int64& nextNoteSeq, int& nextPhraseId,
                                Phrase& openPhrase)
    {
        CaptureResult result;

        if (event.isNoteOn)
        {
            const bool nothingOpenYet = openPhrase.notes.empty();
            const double lastOnset = nothingOpenYet ? event.ppq : openPhrase.notes.back().onsetPpq;
            const bool gapExceeded = ! nothingOpenYet && (event.ppq - lastOnset) >= phraseGapBeats;

            if (nothingOpenYet && openPhrase.phraseId < 0)
                openPhrase.phraseId = nextPhraseId++;

            if (gapExceeded)
            {
                closePhrase (openPhrase, holdBars, beatsPerBarNow);
                result.phraseClosed = true;
                result.closedPhrase = openPhrase;

                openPhrase = Phrase {};
                openPhrase.phraseId = nextPhraseId++;
            }

            HeldNote note;
            note.channel = event.channel;
            note.pitch = event.pitch;
            note.velocity = event.velocity;
            note.onsetPpq = event.ppq;
            note.seq = nextNoteSeq++;
            note.phraseId = openPhrase.phraseId;
            openPhrase.notes.push_back (note);
        }
        else
        {
            // FIFO match: oldest still-open HeldNote sharing (channel,pitch).
            for (auto& n : openPhrase.notes)
            {
                if (n.channel == event.channel && n.pitch == event.pitch && ! n.hasNoteOff)
                {
                    n.durationPpq = juce::jmax (0.0, event.ppq - n.onsetPpq);
                    n.hasNoteOff = true;
                    break;
                }
            }
            // No match (including a note whose phrase already closed) -
            // dropped silently, per this function's own doc comment.
        }

        return result;
    }

    // --- Transform vocabulary ------------------------------------------------

    std::vector<HeldNote> applyTranspose (const std::vector<HeldNote>& notes, int semitones)
    {
        std::vector<HeldNote> out = notes;
        for (auto& n : out)
            n.pitch = juce::jlimit (0, 127, n.pitch + semitones);
        return out;
    }

    std::vector<HeldNote> applyInversion (const std::vector<HeldNote>& notes)
    {
        std::vector<HeldNote> out = notes;
        if (out.empty())
            return out;

        const int axis = out.front().pitch;   // the phrase's own anchor note - see this function's own doc comment
        for (auto& n : out)
            n.pitch = juce::jlimit (0, 127, (axis * 2) - n.pitch);
        return out;
    }

    std::vector<HeldNote> applyRetrograde (const std::vector<HeldNote>& notes,
                                           double phraseStartPpq, double phraseEndPpq)
    {
        std::vector<HeldNote> out = notes;
        for (auto& n : out)
            n.onsetPpq = phraseStartPpq + phraseEndPpq - (n.onsetPpq + n.durationPpq);

        // Reflecting each onset around the phrase midpoint also reverses
        // their relative ORDER (the last note's new onset is smallest) -
        // re-sort so `notes` stays in onset order, matching every other
        // phrase's own invariant (see Phrase's doc comment).
        std::sort (out.begin(), out.end(), [] (const HeldNote& a, const HeldNote& b)
        {
            return a.onsetPpq < b.onsetPpq;
        });
        return out;
    }

    std::vector<HeldNote> applyTransform (const std::vector<HeldNote>& notes, int transformKind,
                                          double phraseStartPpq, double phraseEndPpq, int transposeSemitones)
    {
        switch (transformKind)
        {
            case kTransformTranspose:  return applyTranspose (notes, transposeSemitones);
            case kTransformRetrograde: return applyRetrograde (notes, phraseStartPpq, phraseEndPpq);
            case kTransformInversion:  return applyInversion (notes);
            case kTransformNone:
            default:                   return notes;
        }
    }

    // --- Restlessness-driven proposal (see this file's header) ---------------

    juce::uint32 fnv1aHash (int a, int b, int salt)
    {
        juce::uint32 h = 2166136261u;

        for (int v : { a, b, salt, 0x27d4eb2f })
        {
            h ^= static_cast<juce::uint32> (v & 0xff);          h *= 16777619u;
            h ^= static_cast<juce::uint32> ((v >> 8) & 0xff);   h *= 16777619u;
            h ^= static_cast<juce::uint32> ((v >> 16) & 0xff);  h *= 16777619u;
        }

        h ^= h >> 15; h *= 2246822519u;
        h ^= h >> 13; h *= 3266489917u;
        h ^= h >> 16;
        return h;
    }

    float hashUnit (int a, int b, int salt)
    {
        return static_cast<float> (fnv1aHash (a, b, salt) & 0xffffffu) / static_cast<float> (0xffffff);
    }

    TransformProposal proposeTransform (int instanceSeed, int phraseCounter, float restlessness)
    {
        restlessness = juce::jlimit (0.0f, 1.0f, restlessness);

        TransformProposal proposal;
        proposal.applyAny = hashUnit (instanceSeed, phraseCounter, 1) < restlessness;
        if (! proposal.applyAny)
            return proposal;

        const juce::uint32 h = fnv1aHash (instanceSeed, phraseCounter, 2);
        proposal.transformKind = 1 + static_cast<int> (h % 3u);   // 1=Transpose,2=Retrograde,3=Inversion
        return proposal;
    }
}
