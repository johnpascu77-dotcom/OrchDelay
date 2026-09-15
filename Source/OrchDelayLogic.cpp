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

    void closePhrase (Phrase& phrase, int holdBars, double beatsPerBarNow, double nowPpq)
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

        // Anchored to phraseEnd, not phraseStart - see this function's own
        // doc comment. Also floored at `nowPpq` (the moment of closure
        // itself): a phrase can't close until phraseGapBeats of rest has
        // elapsed past its own end, so for a short holdBars (comparable to
        // or shorter than phraseGapBeats), phraseEnd + holdBars*bar could
        // otherwise land BEFORE the phrase is even known to be closed - an
        // echo can never start before its own source material exists.
        const double holdTargetPpq = phrase.phraseEndPpq + juce::jmax (1, holdBars) * beatsPerBarNow;
        phrase.scheduledFirePpq = juce::jmax (holdTargetPpq, nowPpq);
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
            bool gapExceeded = false;

            if (! nothingOpenYet)
            {
                const HeldNote& last = openPhrase.notes.back();
                // Rest since the last note's END, not onset-to-onset (see
                // this function's own doc comment) - a note still sounding
                // (no note-off yet) has zero rest.
                const double lastEndPpq = last.hasNoteOff ? (last.onsetPpq + last.durationPpq) : event.ppq;
                const double restBeats = juce::jmax (0.0, event.ppq - lastEndPpq);
                gapExceeded = restBeats >= phraseGapBeats;
            }

            if (nothingOpenYet && openPhrase.phraseId < 0)
                openPhrase.phraseId = nextPhraseId++;

            if (gapExceeded)
            {
                closePhrase (openPhrase, holdBars, beatsPerBarNow, event.ppq);
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

    CaptureResult checkPhraseTimeout (double nowPpq, double phraseGapBeats,
                                      int holdBars, double beatsPerBarNow,
                                      Phrase& openPhrase)
    {
        CaptureResult result;

        if (openPhrase.notes.empty())
            return result;

        const HeldNote& last = openPhrase.notes.back();
        if (! last.hasNoteOff)
            return result;   // still sounding - not past yet

        const double lastEndPpq = last.onsetPpq + last.durationPpq;
        if ((nowPpq - lastEndPpq) < phraseGapBeats)
            return result;

        closePhrase (openPhrase, holdBars, beatsPerBarNow, nowPpq);
        result.phraseClosed = true;
        result.closedPhrase = openPhrase;
        openPhrase = Phrase {};

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

    std::vector<HeldNote> applyRotation (const std::vector<HeldNote>& notes, int steps)
    {
        std::vector<HeldNote> out = notes;
        const int n = static_cast<int> (notes.size());
        if (n <= 1)
            return out;   // a 0/1-note "loop" has nothing to rotate, same as MPL

        const int effectiveSteps = ((steps % n) + n) % n;

        for (int i = 0; i < n; ++i)
        {
            const int sourceIndex = ((i - effectiveSteps) % n + n) % n;
            // Slot i keeps its OWN onset/duration (the rhythmic skeleton) -
            // only the CONTENT (pitch/velocity/channel) is reassigned from
            // the source slot, matching MPL's own "which stored step plays
            // here" semantics.
            out[static_cast<size_t> (i)].pitch = notes[static_cast<size_t> (sourceIndex)].pitch;
            out[static_cast<size_t> (i)].velocity = notes[static_cast<size_t> (sourceIndex)].velocity;
            out[static_cast<size_t> (i)].channel = notes[static_cast<size_t> (sourceIndex)].channel;
        }

        return out;
    }

    std::vector<HeldNote> applyLength (const std::vector<HeldNote>& notes, float lengthPercent)
    {
        if (notes.empty())
            return notes;

        const float clamped = juce::jlimit (0.0f, 100.0f, lengthPercent);
        const int keepCount = juce::jlimit (1, static_cast<int> (notes.size()),
                                            juce::roundToInt (static_cast<float> (notes.size()) * clamped / 100.0f));

        return std::vector<HeldNote> (notes.begin(), notes.begin() + keepCount);
    }

    std::vector<HeldNote> applyM7 (const std::vector<HeldNote>& notes)
    {
        std::vector<HeldNote> out = notes;
        for (auto& n : out)
        {
            const int pitchClass = ((n.pitch % 12) + 12) % 12;
            const int m7PitchClass = (pitchClass * 7) % 12;
            n.pitch = juce::jlimit (0, 127, n.pitch - pitchClass + m7PitchClass);
        }
        return out;
    }

    std::vector<HeldNote> applyStretch (const std::vector<HeldNote>& notes, double phraseStartPpq,
                                        float stretchPercent)
    {
        std::vector<HeldNote> out = notes;
        const double factor = static_cast<double> (juce::jlimit (25.0f, 400.0f, stretchPercent)) / 100.0;

        for (auto& n : out)
        {
            const double relativeOnset = n.onsetPpq - phraseStartPpq;
            n.onsetPpq = phraseStartPpq + relativeOnset * factor;
            n.durationPpq = juce::jmax (0.0, n.durationPpq * factor);
        }

        return out;
    }

    std::vector<HeldNote> applyIntervalScale (const std::vector<HeldNote>& notes, float scalePercent)
    {
        std::vector<HeldNote> out = notes;
        if (out.empty())
            return out;

        const int anchor = out.front().pitch;
        const float factor = juce::jlimit (0.0f, 300.0f, scalePercent) / 100.0f;

        for (auto& n : out)
        {
            const int interval = n.pitch - anchor;
            n.pitch = juce::jlimit (0, 127, anchor + juce::roundToInt (static_cast<float> (interval) * factor));
        }

        return out;
    }

    std::vector<HeldNote> applyTransform (const std::vector<HeldNote>& notes, int transformKind,
                                          double phraseStartPpq, double phraseEndPpq, int transposeSemitones,
                                          int rotationSteps, float lengthPercent, float stretchPercent,
                                          float intervalScalePercent)
    {
        switch (transformKind)
        {
            case kTransformTranspose:  return applyTranspose (notes, transposeSemitones);
            case kTransformRetrograde: return applyRetrograde (notes, phraseStartPpq, phraseEndPpq);
            case kTransformInversion:  return applyInversion (notes);
            case kTransformRotation:   return applyRotation (notes, rotationSteps);
            case kTransformLength:     return applyLength (notes, lengthPercent);
            case kTransformM7:         return applyM7 (notes);
            case kTransformStretch:    return applyStretch (notes, phraseStartPpq, stretchPercent);
            case kTransformInterval:   return applyIntervalScale (notes, intervalScalePercent);
            case kTransformNone:
            default:                   return notes;
        }
    }

    std::vector<ScheduledNote> buildOutputNotes (const Phrase& phrase, int transposeSemitones,
                                                 int rotationSteps, float lengthPercent, float stretchPercent,
                                                 float intervalScalePercent)
    {
        const auto transformed = applyTransform (phrase.notes, phrase.chosenTransform,
                                                  phrase.phraseStartPpq, phrase.phraseEndPpq,
                                                  transposeSemitones, rotationSteps, lengthPercent,
                                                  stretchPercent, intervalScalePercent);

        std::vector<ScheduledNote> out;
        out.reserve (transformed.size());

        for (const auto& n : transformed)
        {
            ScheduledNote sn;
            sn.channel = n.channel;
            sn.pitch = n.pitch;
            sn.velocity = n.velocity;
            sn.outputOnsetPpq = phrase.scheduledFirePpq + (n.onsetPpq - phrase.phraseStartPpq);
            sn.outputOffPpq = sn.outputOnsetPpq + n.durationPpq;
            sn.seq = n.seq;
            out.push_back (sn);
        }

        return out;
    }

    void shiftOutputNotes (Phrase& phrase, double shiftPpq)
    {
        for (auto& note : phrase.outputNotes)
        {
            note.outputOnsetPpq += shiftPpq;
            note.outputOffPpq += shiftPpq;
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
        // 1=Transpose,2=Retrograde,3=Inversion,4=Rotation,5=Length,6=M7,7=Stretch,8=Interval
        proposal.transformKind = 1 + static_cast<int> (h % 8u);
        return proposal;
    }

    PhraseFeatures computePhraseFeatures (const std::vector<HeldNote>& notes)
    {
        PhraseFeatures f;
        f.noteCount = static_cast<int> (notes.size());
        if (notes.empty())
            return f;

        int minPitch = notes.front().pitch;
        int maxPitch = notes.front().pitch;
        for (const auto& n : notes)
        {
            minPitch = juce::jmin (minPitch, n.pitch);
            maxPitch = juce::jmax (maxPitch, n.pitch);
        }
        f.pitchSpread = maxPitch - minPitch;

        // Notes are always in onset order (Phrase's own invariant), so the
        // span from first to last onset is a cheap, adequate density proxy -
        // a floor avoids dividing by (near) zero for a single very short
        // phrase.
        const double spanBeats = notes.size() > 1 ? (notes.back().onsetPpq - notes.front().onsetPpq) : 0.0;
        f.density = static_cast<float> (f.noteCount) / static_cast<float> (juce::jmax (0.25, spanBeats));

        return f;
    }

    TransformProposal proposeWeightedTransform (int instanceSeed, int phraseCounter, float restlessness,
                                                const PhraseFeatures& features)
    {
        restlessness = juce::jlimit (0.0f, 1.0f, restlessness);

        TransformProposal proposal;
        proposal.applyAny = hashUnit (instanceSeed, phraseCounter, 1) < restlessness;
        if (! proposal.applyAny)
            return proposal;

        const float normalizedDensity = juce::jlimit (0.0f, 1.0f, features.density / 4.0f);
        const float normalizedSparsity = 1.0f - normalizedDensity;
        const float normalizedSpread = juce::jlimit (0.0f, 1.0f, static_cast<float> (features.pitchSpread) / 24.0f);
        const float normalizedCount = juce::jlimit (0.0f, 1.0f, static_cast<float> (features.noteCount - 1) / 7.0f);

        // Base weight 1.0 for every transform, boosted up to +1.0 for the
        // ones that suit this phrase's own content - see this function's
        // own doc comment for the reasoning behind each pairing. Order
        // matches TransformKind 1..8.
        const float weights[8] = {
            1.0f,                        // 1 Transpose
            1.0f,                        // 2 Retrograde
            1.0f + normalizedSpread,     // 3 Inversion
            1.0f + normalizedCount,      // 4 Rotation
            1.0f + normalizedDensity,    // 5 Length
            1.0f,                        // 6 M7
            1.0f + normalizedSparsity,   // 7 Stretch
            1.0f + normalizedSpread      // 8 Interval
        };

        float total = 0.0f;
        for (float w : weights)
            total += w;

        const float target = hashUnit (instanceSeed, phraseCounter, 10) * total;   // salt 10
        float cumulative = 0.0f;
        int chosen = 1;
        for (int i = 0; i < 8; ++i)
        {
            cumulative += weights[i];
            if (target < cumulative)
            {
                chosen = i + 1;
                break;
            }
        }

        proposal.transformKind = chosen;
        return proposal;
    }

    float computePhraseInterest (const std::vector<HeldNote>& notes)
    {
        if (notes.size() < 2)
            return 0.0f;   // a single note is about as low-interest as it gets structurally

        std::vector<int> pitches;
        pitches.reserve (notes.size());
        for (const auto& n : notes)
            pitches.push_back (n.pitch);
        std::sort (pitches.begin(), pitches.end());
        const auto distinctCount = std::distance (pitches.begin(), std::unique (pitches.begin(), pitches.end()));

        const float countFactor = juce::jlimit (0.0f, 1.0f, static_cast<float> (notes.size() - 1) / 5.0f);
        const float varietyFactor = static_cast<float> (distinctCount) / static_cast<float> (notes.size());

        int minPitch = notes.front().pitch;
        int maxPitch = notes.front().pitch;
        for (const auto& n : notes)
        {
            minPitch = juce::jmin (minPitch, n.pitch);
            maxPitch = juce::jmax (maxPitch, n.pitch);
        }
        const float spreadFactor = juce::jlimit (0.0f, 1.0f, static_cast<float> (maxPitch - minPitch) / 12.0f);

        return (countFactor + varietyFactor + spreadFactor) / 3.0f;
    }

    MemoryCallbackDecision resolveMemoryCallback (int instanceSeed, int phraseCounter,
                                                  float callbackProbabilityPercent, int poolSize)
    {
        MemoryCallbackDecision decision;
        if (poolSize <= 0)
            return decision;

        const float probability = juce::jlimit (0.0f, 1.0f, callbackProbabilityPercent / 100.0f);
        decision.useCallback = hashUnit (instanceSeed, phraseCounter, 11) < probability;   // salt 11
        if (! decision.useCallback)
            return decision;

        const juce::uint32 h = fnv1aHash (instanceSeed, phraseCounter, 12);   // salt 12
        decision.poolIndex = static_cast<int> (h % static_cast<juce::uint32> (poolSize));
        return decision;
    }

    int resolveRandomTransposeSemitones (int instanceSeed, int phraseCounter, int rangeSemitones)
    {
        const int bound = juce::jlimit (0, 48, rangeSemitones < 0 ? -rangeSemitones : rangeSemitones);
        if (bound == 0)
            return 0;

        const float unit = hashUnit (instanceSeed, phraseCounter, 3);   // salt 3 - independent of proposeTransform's 1/2
        const int span = 2 * bound + 1;   // inclusive [-bound, +bound]
        const int offset = juce::jlimit (0, span - 1, static_cast<int> (unit * static_cast<float> (span)));
        return -bound + offset;
    }

    int resolveRandomRotationSteps (int instanceSeed, int phraseCounter, int rangeSteps)
    {
        const int bound = rangeSteps < 0 ? -rangeSteps : rangeSteps;
        if (bound == 0)
            return 0;

        const float unit = hashUnit (instanceSeed, phraseCounter, 4);   // salt 4
        const int span = 2 * bound + 1;
        const int offset = juce::jlimit (0, span - 1, static_cast<int> (unit * static_cast<float> (span)));
        return -bound + offset;
    }

    float resolveRandomLengthPercent (int instanceSeed, int phraseCounter, float ceilingPercent)
    {
        const float ceiling = juce::jlimit (1.0f, 100.0f, ceilingPercent);
        if (ceiling <= 1.0f)
            return 1.0f;

        const float unit = hashUnit (instanceSeed, phraseCounter, 5);   // salt 5
        return juce::jlimit (1.0f, 100.0f, 1.0f + unit * (ceiling - 1.0f));
    }

    float resolveRandomStretchPercent (int instanceSeed, int phraseCounter, float boundPercent)
    {
        const float bound = juce::jlimit (25.0f, 400.0f, boundPercent);
        const float diff = bound > 100.0f ? (bound - 100.0f) : (100.0f - bound);
        if (diff < 1e-6f)
            return 100.0f;

        const float lo = juce::jmin (100.0f, bound);
        const float hi = juce::jmax (100.0f, bound);
        const float unit = hashUnit (instanceSeed, phraseCounter, 6);   // salt 6
        return lo + unit * (hi - lo);
    }

    const std::vector<float>& quantizedStretchRatios()
    {
        static const std::vector<float> ratios {
            25.0f, 100.0f / 3.0f, 50.0f, 200.0f / 3.0f, 75.0f, 100.0f,
            400.0f / 3.0f, 150.0f, 200.0f, 300.0f, 400.0f
        };
        return ratios;
    }

    float snapToQuantizedStretch (float percent)
    {
        const auto& ratios = quantizedStretchRatios();
        float best = ratios.front();
        float bestDiff = percent > best ? (percent - best) : (best - percent);

        for (float candidate : ratios)
        {
            const float diff = percent > candidate ? (percent - candidate) : (candidate - percent);
            if (diff < bestDiff)
            {
                bestDiff = diff;
                best = candidate;
            }
        }

        return best;
    }

    float resolveRandomQuantizedStretchPercent (int instanceSeed, int phraseCounter, float boundPercent)
    {
        const float bound = juce::jlimit (25.0f, 400.0f, boundPercent);
        const float diff = bound > 100.0f ? (bound - 100.0f) : (100.0f - bound);
        if (diff < 1e-6f)
            return 100.0f;

        const float lo = juce::jmin (100.0f, bound);
        const float hi = juce::jmax (100.0f, bound);

        std::vector<float> candidates;
        for (float r : quantizedStretchRatios())
            if (r >= lo - 1e-3f && r <= hi + 1e-3f)
                candidates.push_back (r);
        if (candidates.empty())
            candidates.push_back (100.0f);

        const juce::uint32 h = fnv1aHash (instanceSeed, phraseCounter, 7);   // salt 7
        return candidates[h % static_cast<juce::uint32> (candidates.size())];
    }

    int resolveRandomHoldBars (int instanceSeed, int phraseCounter, int ceilingBars)
    {
        const int ceiling = juce::jlimit (1, 16, ceilingBars);
        if (ceiling <= 1)
            return 1;

        const float unit = hashUnit (instanceSeed, phraseCounter, 8);   // salt 8
        const int offset = juce::jlimit (0, ceiling - 1, static_cast<int> (unit * static_cast<float> (ceiling)));
        return 1 + offset;
    }

    float resolveRandomIntervalPercent (int instanceSeed, int phraseCounter, float boundPercent)
    {
        const float bound = juce::jlimit (0.0f, 300.0f, boundPercent);
        const float diff = bound > 100.0f ? (bound - 100.0f) : (100.0f - bound);
        if (diff < 1e-6f)
            return 100.0f;

        const float lo = juce::jmin (100.0f, bound);
        const float hi = juce::jmax (100.0f, bound);
        const float unit = hashUnit (instanceSeed, phraseCounter, 9);   // salt 9
        return lo + unit * (hi - lo);
    }
}
