#include "OrchDelayLogic.h"

#include <cmath>
#include <iostream>
#include <string>

namespace
{
    int failures = 0;

    void check (bool condition, const std::string& label)
    {
        if (condition)
        {
            std::cout << "[PASS] " << label << "\n";
        }
        else
        {
            std::cerr << "[FAIL] " << label << "\n";
            ++failures;
        }
    }

    odly::HeldNote makeNote (double onset, double duration, int pitch, int channel = 1)
    {
        odly::HeldNote n;
        n.onsetPpq = onset;
        n.durationPpq = duration;
        n.pitch = pitch;
        n.channel = channel;
        n.hasNoteOff = true;
        return n;
    }
}

int main()
{
    // --- beatsPerBar across several meters ---------------------------------
    {
        check (std::abs (odly::beatsPerBar (4, 4) - 4.0) < 1e-9, "beatsPerBar 4/4 = 4.0");
        check (std::abs (odly::beatsPerBar (3, 4) - 3.0) < 1e-9, "beatsPerBar 3/4 = 3.0");
        check (std::abs (odly::beatsPerBar (6, 8) - 3.0) < 1e-9, "beatsPerBar 6/8 = 3.0 quarter-note-equivalent");
        check (std::abs (odly::beatsPerBar (5, 8) - 2.5) < 1e-9, "beatsPerBar 5/8 = 2.5");
        check (std::abs (odly::beatsPerBar (4, 0) - 4.0) < 1e-9, "beatsPerBar: degenerate denominator falls back to 4.0");
    }

    // --- phrase-boundary grouping via captureEvent --------------------------
    {
        juce::int64 seq = 1;
        int phraseId = 0;
        odly::Phrase open;

        // Two note-ons 0.5 beats apart (below the 1.0-beat default gap) stay one phrase.
        auto r1 = odly::captureEvent ({ true, 1, 60, 100, 0.0 }, 1.0, 4, 4.0, seq, phraseId, open);
        check (! r1.phraseClosed, "grouping: first note never closes anything");
        auto r2 = odly::captureEvent ({ true, 1, 62, 100, 0.5 }, 1.0, 4, 4.0, seq, phraseId, open);
        check (! r2.phraseClosed && open.notes.size() == 2, "grouping: gap below threshold stays in the same phrase");

        // A gap AT the threshold (1.0 beat) closes it.
        auto r3 = odly::captureEvent ({ true, 1, 64, 100, 1.5 }, 1.0, 4, 4.0, seq, phraseId, open);
        check (r3.phraseClosed && r3.closedPhrase.notes.size() == 2, "grouping: gap at threshold closes the phrase (2 notes)");
        check (open.notes.size() == 1 && open.notes.front().pitch == 64, "grouping: the new note starts a fresh phrase");
    }
    {
        // A single isolated note (transport stop mid-phrase, or closed by the
        // caller directly) is its own valid one-note phrase.
        juce::int64 seq = 1;
        int phraseId = 0;
        odly::Phrase open;
        odly::captureEvent ({ true, 1, 60, 100, 0.0 }, 1.0, 4, 4.0, seq, phraseId, open);
        check (open.notes.size() == 1, "grouping: a single note is a valid one-note in-progress phrase");
    }
    {
        // Zero-gap-tolerance edge case: threshold 0.0 closes on ANY gap, even a tiny one.
        juce::int64 seq = 1;
        int phraseId = 0;
        odly::Phrase open;
        odly::captureEvent ({ true, 1, 60, 100, 0.0 }, 0.0, 4, 4.0, seq, phraseId, open);
        auto r = odly::captureEvent ({ true, 1, 62, 100, 0.001 }, 0.0, 4, 4.0, seq, phraseId, open);
        check (r.phraseClosed, "grouping: threshold 0.0 closes on any nonzero gap");
    }

    // --- note-off matching / seq discipline ---------------------------------
    {
        // Reproduces OrchPiano's own historical bug shape: two overlapping
        // same-pitch, same-channel notes must resolve via FIFO seq order, not
        // (channel,pitch) alone.
        juce::int64 seq = 1;
        int phraseId = 0;
        odly::Phrase open;
        odly::captureEvent ({ true, 1, 60, 100, 0.0 }, 1.0, 4, 4.0, seq, phraseId, open);   // seq 1
        odly::captureEvent ({ true, 1, 60, 100, 0.1 }, 1.0, 4, 4.0, seq, phraseId, open);   // seq 2, same pitch, still open
        check (open.notes.size() == 2 && ! open.notes[0].hasNoteOff && ! open.notes[1].hasNoteOff,
              "seq discipline: two overlapping same-pitch notes both stay open");

        odly::captureEvent ({ false, 1, 60, 0, 0.5 }, 1.0, 4, 4.0, seq, phraseId, open);    // first note-off
        check (open.notes[0].hasNoteOff && ! open.notes[1].hasNoteOff,
              "seq discipline: a note-off matches the OLDEST open occurrence first (FIFO), not an arbitrary one");

        // A stray note-off (no open match) is dropped silently, not crashing/asserting.
        odly::captureEvent ({ false, 1, 90, 0, 0.6 }, 1.0, 4, 4.0, seq, phraseId, open);
        check (open.notes.size() == 2, "seq discipline: a stray note-off with no match is dropped silently");
    }

    // --- closePhrase: fallback duration for a straddling held note ---------
    {
        odly::Phrase p;
        p.notes.push_back (makeNote (0.0, 0.0, 60));
        p.notes.back().hasNoteOff = false;   // still held when the phrase closes
        p.notes.push_back (makeNote (0.5, 0.5, 62));
        odly::closePhrase (p, 4, 4.0);
        check (p.notes[0].hasNoteOff && std::abs (p.notes[0].durationPpq - 0.5) < 1e-9,
              "closePhrase: a straddling held note gets the 0.5-beat fallback duration and closes");
        check (p.closed, "closePhrase: sets closed=true");
    }

    // --- scheduledFirePpq: meter frozen at closure time ----------------------
    {
        odly::Phrase p;
        p.notes.push_back (makeNote (10.0, 1.0, 60));
        odly::closePhrase (p, 4, 4.0);   // 4 bars of 4/4 = 16 quarter notes
        check (std::abs (p.scheduledFirePpq - (10.0 + 16.0)) < 1e-9,
              "scheduledFirePpq: phraseStart + holdBars*beatsPerBar, using the meter at closure");
    }

    // --- Transpose clamping at both ends -------------------------------------
    {
        std::vector<odly::HeldNote> notes { makeNote (0.0, 1.0, 5), makeNote (1.0, 1.0, 125) };
        auto out = odly::applyTranspose (notes, -20);
        check (out[0].pitch == 0, "transpose: clamps at the low end (5-20 -> 0)");
        auto out2 = odly::applyTranspose (notes, 20);
        check (out2[1].pitch == 127, "transpose: clamps at the high end (125+20 -> 127)");
    }

    // --- Inversion around the phrase's own first note, not a fixed axis=60 --
    {
        std::vector<odly::HeldNote> notes { makeNote (0.0, 1.0, 40), makeNote (1.0, 1.0, 45), makeNote (2.0, 1.0, 43) };
        auto out = odly::applyInversion (notes);
        // axis = 40 (the phrase's own first note): inverted = 40*2 - pitch
        check (out[0].pitch == 40, "inversion: the anchor note itself stays put (mirrors onto itself)");
        check (out[1].pitch == 35, "inversion: 45 mirrors to 35 around axis=40 (the phrase's own first note)");
        check (out[2].pitch == 37, "inversion: 43 mirrors to 37 around axis=40");
        // This assertion would FAIL under MPL's own fixed axis=60 convention
        // (45 would mirror to 75, not 35) - makes the deliberate divergence
        // machine-verifiable, per the design doc.
        check (out[1].pitch != (60 * 2 - 45), "inversion: confirms this is NOT MPL's fixed axis=60 convention");
    }

    // --- Retrograde: exact whole-phrase time-reversal formula ---------------
    {
        // Phrase spans ppq [0, 3]: note A [0,1), note B [1,2), note C [2,3).
        std::vector<odly::HeldNote> notes {
            makeNote (0.0, 1.0, 60), makeNote (1.0, 1.0, 62), makeNote (2.0, 1.0, 64)
        };
        auto out = odly::applyRetrograde (notes, 0.0, 3.0);
        check (out.size() == 3, "retrograde: same note count");
        // newOnset = phraseStart + phraseEnd - (onset + duration) = 0 + 3 - (onset+duration)
        // A(0,1)->newOnset=2, B(1,2)->newOnset=1, C(2,3)->newOnset=0 - and re-sorted by onset.
        check (std::abs (out[0].onsetPpq - 0.0) < 1e-9 && out[0].pitch == 64,
              "retrograde: the LAST captured note (C, pitch 64) now plays FIRST");
        check (std::abs (out[1].onsetPpq - 1.0) < 1e-9 && out[1].pitch == 62,
              "retrograde: the middle note keeps its own duration, reflected onset");
        check (std::abs (out[2].onsetPpq - 2.0) < 1e-9 && out[2].pitch == 60,
              "retrograde: the FIRST captured note (A, pitch 60) now plays LAST");
    }

    // --- proposeTransform determinism + cross-instance divergence -----------
    {
        auto p1 = odly::proposeTransform (42, 7, 0.5f);
        auto p2 = odly::proposeTransform (42, 7, 0.5f);
        check (p1.applyAny == p2.applyAny && p1.transformKind == p2.transformKind,
              "proposeTransform: identical inputs always produce identical outputs (determinism)");

        int differing = 0;
        for (int seedB = 0; seedB < 20; ++seedB)
        {
            auto a = odly::proposeTransform (1, seedB, 1.0f);
            auto b = odly::proposeTransform (2, seedB, 1.0f);
            if (a.transformKind != b.transformKind)
                ++differing;
        }
        check (differing > 0, "proposeTransform: different instanceSeed values diverge across a sample");
    }

    // --- restlessness threshold sanity --------------------------------------
    {
        int appliedAtZero = 0, appliedAtOne = 0;
        const int trials = 4000;
        for (int i = 0; i < trials; ++i)
        {
            if (odly::proposeTransform (11, i, 0.0f).applyAny) ++appliedAtZero;
            if (odly::proposeTransform (11, i, 1.0f).applyAny) ++appliedAtOne;
        }
        check (appliedAtZero == 0, "restlessness=0: NEVER proposes a transform (always verbatim)");
        check (appliedAtOne == trials, "restlessness=1: ALWAYS proposes a transform");

        int appliedAtHalf = 0;
        for (int i = 0; i < trials; ++i)
            if (odly::proposeTransform (11, i, 0.5f).applyAny) ++appliedAtHalf;
        const double rate = static_cast<double> (appliedAtHalf) / trials;
        check (rate > 0.40 && rate < 0.60, "restlessness=0.5: observed apply-rate within a reasonable band of 50%");
    }

    // --- applyTransform dispatch ----------------------------------------------
    {
        std::vector<odly::HeldNote> notes { makeNote (0.0, 1.0, 60) };
        auto same = odly::applyTransform (notes, odly::kTransformNone, 0.0, 1.0, 12);
        check (same[0].pitch == 60, "applyTransform: kTransformNone returns input unchanged");
        auto up = odly::applyTransform (notes, odly::kTransformTranspose, 0.0, 1.0, 12);
        check (up[0].pitch == 72, "applyTransform: dispatches to Transpose correctly");
    }

    std::cout << "-------------------------\n";
    if (failures == 0)
    {
        std::cout << "[PASS] OrchDelayLogicCheck passed.\n";
        return 0;
    }

    std::cerr << "[FAIL] " << failures << " failure(s).\n";
    return 1;
}
