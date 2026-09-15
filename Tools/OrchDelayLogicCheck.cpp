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
    // Gap is measured as REST since the previous note's END, not onset-to-
    // onset - every scenario below sends an explicit note-off to establish
    // that end point before checking the next note-on's gap.
    {
        juce::int64 seq = 1;
        int phraseId = 0;
        odly::Phrase open;

        auto r1 = odly::captureEvent ({ true, 1, 60, 100, 0.0 }, 1.0, 4, 4.0, seq, phraseId, open);
        check (! r1.phraseClosed, "grouping: first note never closes anything");
        odly::captureEvent ({ false, 1, 60, 0, 0.9 }, 1.0, 4, 4.0, seq, phraseId, open);   // note ends at 0.9

        // Onset at 1.0 -> rest = 1.0 - 0.9 = 0.1, well below the 1.0 threshold,
        // even though onset-to-onset (1.0) sits right AT the old (buggy) check.
        auto r2 = odly::captureEvent ({ true, 1, 62, 100, 1.0 }, 1.0, 4, 4.0, seq, phraseId, open);
        check (! r2.phraseClosed && open.notes.size() == 2,
              "grouping: rest below threshold stays in the same phrase, even with a ~1-beat onset gap");
        odly::captureEvent ({ false, 1, 62, 0, 1.9 }, 1.0, 4, 4.0, seq, phraseId, open);   // note ends at 1.9

        // Onset at 3.0 -> rest = 3.0 - 1.9 = 1.1, at/above the threshold.
        auto r3 = odly::captureEvent ({ true, 1, 64, 100, 3.0 }, 1.0, 4, 4.0, seq, phraseId, open);
        check (r3.phraseClosed && r3.closedPhrase.notes.size() == 2, "grouping: rest at/above threshold closes the phrase (2 notes)");
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
        // Zero-gap-tolerance edge case: threshold 0.0 closes on ANY rest, even a tiny one.
        juce::int64 seq = 1;
        int phraseId = 0;
        odly::Phrase open;
        odly::captureEvent ({ true, 1, 60, 100, 0.0 }, 0.0, 4, 4.0, seq, phraseId, open);
        odly::captureEvent ({ false, 1, 60, 0, 0.0005 }, 0.0, 4, 4.0, seq, phraseId, open);
        auto r = odly::captureEvent ({ true, 1, 62, 100, 0.001 }, 0.0, 4, 4.0, seq, phraseId, open);
        check (r.phraseClosed, "grouping: threshold 0.0 closes on any nonzero rest");
    }
    {
        // Regression test for the real live bug ("4 played, only 3 echoed"):
        // 4 near-legato quarter notes (each ~0.95 beats long) spaced 1.0 beat
        // apart in ONSET, with phraseGapBeats == 1.0 (the onset spacing
        // itself). Under the old onset-to-onset check this fractured into 4
        // separate one-note phrases; the true rest between notes (~0.05
        // beat) is far below the threshold, so it must stay ONE phrase.
        juce::int64 seq = 1;
        int phraseId = 0;
        odly::Phrase open;
        const double phraseGap = 1.0;

        for (int i = 0; i < 4; ++i)
        {
            const double onset = static_cast<double> (i);
            odly::captureEvent ({ true, 1, 60 + i, 100, onset }, phraseGap, 4, 4.0, seq, phraseId, open);
            odly::captureEvent ({ false, 1, 60 + i, 0, onset + 0.95 }, phraseGap, 4, 4.0, seq, phraseId, open);
        }
        check (open.notes.size() == 4,
              "grouping: 4 near-legato quarter notes at 1-beat spacing stay ONE phrase, not 4 (the real live bug)");
    }

    // --- checkPhraseTimeout: closes a finished phrase with no follow-up note ---
    {
        odly::Phrase open;
        juce::int64 seq = 1;
        int phraseId = 0;
        odly::captureEvent ({ true, 1, 60, 100, 0.0 }, 1.0, 4, 4.0, seq, phraseId, open);
        odly::captureEvent ({ false, 1, 60, 0, 0.9 }, 1.0, 4, 4.0, seq, phraseId, open);   // ends at 0.9

        auto tooSoon = odly::checkPhraseTimeout (1.5, 1.0, 4, 4.0, open);   // rest so far = 0.6
        check (! tooSoon.phraseClosed && open.notes.size() == 1,
              "checkPhraseTimeout: not yet closed while rest is still below the threshold");

        auto closed = odly::checkPhraseTimeout (2.0, 1.0, 4, 4.0, open);   // rest so far = 1.1
        check (closed.phraseClosed && closed.closedPhrase.notes.size() == 1 && open.notes.empty(),
              "checkPhraseTimeout: closes on its own once the rest threshold passes, with NO follow-up note - "
              "the fix for a finite clip's trailing phrase never firing back");
    }
    {
        // Must not fire while the last note is still sounding (no note-off) -
        // can't call a rest "elapsed" for a note that hasn't ended.
        odly::Phrase open;
        juce::int64 seq = 1;
        int phraseId = 0;
        odly::captureEvent ({ true, 1, 60, 100, 0.0 }, 1.0, 4, 4.0, seq, phraseId, open);
        auto r = odly::checkPhraseTimeout (100.0, 1.0, 4, 4.0, open);
        check (! r.phraseClosed, "checkPhraseTimeout: never closes while the last note is still held (no note-off)");
    }
    {
        odly::Phrase empty;
        auto r = odly::checkPhraseTimeout (100.0, 1.0, 4, 4.0, empty);
        check (! r.phraseClosed, "checkPhraseTimeout: no-op on an empty open phrase");
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
        odly::closePhrase (p, 4, 4.0, 1.0);
        check (p.notes[0].hasNoteOff && std::abs (p.notes[0].durationPpq - 0.5) < 1e-9,
              "closePhrase: a straddling held note gets the 0.5-beat fallback duration and closes");
        check (p.closed, "closePhrase: sets closed=true");
    }

    // --- scheduledFirePpq: anchored to phraseEnd, meter frozen at closure ---
    {
        odly::Phrase p;
        p.notes.push_back (makeNote (10.0, 1.0, 60));   // phraseEndPpq = 11.0
        odly::closePhrase (p, 4, 4.0, 12.0);   // 4 bars of 4/4 = 16 quarter notes; closure at ppq 12
        check (std::abs (p.scheduledFirePpq - (11.0 + 16.0)) < 1e-9,
              "scheduledFirePpq: phraseEnd + holdBars*beatsPerBar - anchored to when the phrase ENDS, "
              "not when it started - using the meter at closure");
    }

    // --- scheduledFirePpq: floored at closure time for a short holdBars -----
    // Regression test for the real live bug ("first note fires almost
    // together with the second" at Hold Bars=1): with a short holdBars
    // (comparable to or shorter than the rest needed to even detect
    // closure), phraseEnd + holdBars*bar could land BEFORE the phrase is
    // actually known to be closed - an echo can never start before its own
    // source material exists.
    {
        odly::Phrase p;
        p.notes.push_back (makeNote (0.0, 0.5, 60));   // phraseEndPpq = 0.5
        // 1 bar of 4/4 = 4 beats -> naive target = 0.5+4 = 4.5, but closure
        // itself doesn't happen until ppq 6.0 here (e.g. a large
        // phraseGapBeats) - scheduledFirePpq must never be earlier than that.
        odly::closePhrase (p, 1, 4.0, 6.0);
        check (std::abs (p.scheduledFirePpq - 6.0) < 1e-9,
              "scheduledFirePpq: floored at the closure-time ppq when phraseEnd+holdBars*bar would be earlier");
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

    // --- buildOutputNotes: each note gets its OWN independent output time ---
    // Regression test for the real live bug: firing a multi-note phrase as
    // one atomic block-clamped operation collapsed the echo into a
    // simultaneous cluster instead of preserving its original rhythm.
    {
        odly::Phrase p;
        p.phraseStartPpq = 0.0;
        p.phraseEndPpq = 4.0;
        p.scheduledFirePpq = 8.0;
        p.chosenTransform = odly::kTransformNone;
        p.notes.push_back (makeNote (0.0, 0.95, 60));
        p.notes.push_back (makeNote (1.0, 0.95, 62));
        p.notes.push_back (makeNote (2.0, 0.95, 64));
        p.notes.push_back (makeNote (3.0, 0.95, 65));

        auto out = odly::buildOutputNotes (p, 0, 0, 100.0f, 100.0f, 100.0f);
        check (out.size() == 4, "buildOutputNotes: same note count as the phrase");
        // Each note's own output onset = scheduledFirePpq + (its own onset - phraseStart) -
        // NOT all collapsed onto scheduledFirePpq itself.
        check (std::abs (out[0].outputOnsetPpq - 8.0) < 1e-9, "buildOutputNotes: note 1 fires exactly at scheduledFirePpq");
        check (std::abs (out[1].outputOnsetPpq - 9.0) < 1e-9, "buildOutputNotes: note 2 fires 1 beat later, not bundled with note 1");
        check (std::abs (out[2].outputOnsetPpq - 10.0) < 1e-9, "buildOutputNotes: note 3 fires 2 beats later");
        check (std::abs (out[3].outputOnsetPpq - 11.0) < 1e-9, "buildOutputNotes: note 4 fires 3 beats later - the phrase's own rhythm is preserved, not clustered");
        check (std::abs (out[0].outputOffPpq - 8.95) < 1e-9, "buildOutputNotes: output note-off preserves the note's own duration");
        check (! out[0].emitted && ! out[1].emitted, "buildOutputNotes: notes start unemitted");
    }

    // --- applyRotation: cyclic reassignment of pitch among onset slots -------
    {
        // 4-note phrase, pitches C E G C (60,64,67,60), onsets 0,1,2,3.
        std::vector<odly::HeldNote> notes {
            makeNote (0.0, 1.0, 60), makeNote (1.0, 1.0, 64), makeNote (2.0, 1.0, 67), makeNote (3.0, 1.0, 60)
        };
        auto out1 = odly::applyRotation (notes, 1);
        // slot i takes its pitch from slot (i-1) mod 4: slot0<-slot3(60), slot1<-slot0(60), slot2<-slot1(64), slot3<-slot2(67)
        check (out1[0].pitch == 60 && out1[1].pitch == 60 && out1[2].pitch == 64 && out1[3].pitch == 67,
              "rotation: steps=1 shifts pitches forward one slot (source = playback - rotation)");
        // onsets/durations (the rhythmic skeleton) are UNCHANGED by rotation.
        check (std::abs (out1[0].onsetPpq - 0.0) < 1e-9 && std::abs (out1[3].onsetPpq - 3.0) < 1e-9,
              "rotation: onsets stay exactly where they were - only pitch content moves");

        auto out0 = odly::applyRotation (notes, 0);
        check (out0[0].pitch == 60 && out0[1].pitch == 64 && out0[2].pitch == 67 && out0[3].pitch == 60,
              "rotation: steps=0 is a no-op");

        auto outWrap = odly::applyRotation (notes, 4);   // a full rotation of a 4-note phrase wraps to itself
        check (outWrap[0].pitch == 60 && outWrap[1].pitch == 64 && outWrap[2].pitch == 67 && outWrap[3].pitch == 60,
              "rotation: steps==noteCount wraps back to a no-op, same as MPL rotating a full loop");

        std::vector<odly::HeldNote> oneNote { makeNote (0.0, 1.0, 60) };
        auto outSingle = odly::applyRotation (oneNote, 5);
        check (outSingle[0].pitch == 60, "rotation: a single-note phrase is always a no-op regardless of amount");
    }

    // --- applyLength: truncates to the first N% of notes by onset order -----
    {
        std::vector<odly::HeldNote> notes {
            makeNote (0.0, 1.0, 60), makeNote (1.0, 1.0, 62), makeNote (2.0, 1.0, 64), makeNote (3.0, 1.0, 65)
        };
        auto out100 = odly::applyLength (notes, 100.0f);
        check (out100.size() == 4, "length: 100% keeps the whole phrase");

        auto out50 = odly::applyLength (notes, 50.0f);
        check (out50.size() == 2 && out50[0].pitch == 60 && out50[1].pitch == 62,
              "length: 50% keeps the FIRST half of the notes, by onset order");

        auto out1 = odly::applyLength (notes, 1.0f);
        check (out1.size() == 1 && out1[0].pitch == 60, "length: a tiny percentage still keeps at least 1 note");

        auto outEmpty = odly::applyLength ({}, 50.0f);
        check (outEmpty.empty(), "length: an empty phrase stays empty, no crash");
    }

    // --- applyM7: pitch-class x7 mod 12, octave register preserved ----------
    {
        std::vector<odly::HeldNote> notes {
            makeNote (0.0, 1.0, 60),   // C4, pitch class 0 -> 0 (fixed point)
            makeNote (1.0, 1.0, 61),   // C#4, pitch class 1 -> 7 (G)
            makeNote (2.0, 1.0, 67)    // G4, pitch class 7 -> 1 (C#)
        };
        auto out = odly::applyM7 (notes);
        check (out[0].pitch == 60, "M7: pitch class 0 (C) is a fixed point");
        check (out[1].pitch == 67, "M7: C#4 (61) maps to G in the SAME octave (67), not just the pitch class");
        check (out[2].pitch == 61, "M7: G4 (67) maps to C# in the same octave (61) - M7 is its own inverse of note 2");

        auto backAgain = odly::applyM7 (out);
        check (backAgain[0].pitch == 60 && backAgain[1].pitch == 61 && backAgain[2].pitch == 67,
              "M7: applying it twice is a no-op (M7 is its own inverse, 7*7 mod 12 == 1) - matches MPL's own convention");
    }

    // --- applyStretch: proportional rescale around the phrase's own start ---
    {
        // Phrase spans [0,4): onsets 0,1,2,3, each 0.9 beats long.
        std::vector<odly::HeldNote> notes {
            makeNote (0.0, 0.9, 60), makeNote (1.0, 0.9, 62), makeNote (2.0, 0.9, 64), makeNote (3.0, 0.9, 65)
        };
        auto out100 = odly::applyStretch (notes, 0.0, 100.0f);
        check (std::abs (out100[1].onsetPpq - 1.0) < 1e-9 && std::abs (out100[1].durationPpq - 0.9) < 1e-9,
              "stretch: 100% leaves onsets and durations unchanged");

        auto out50 = odly::applyStretch (notes, 0.0, 50.0f);
        check (std::abs (out50[0].onsetPpq - 0.0) < 1e-9,
              "stretch: the phrase's own first note (at phraseStart) never moves, regardless of factor");
        check (std::abs (out50[1].onsetPpq - 0.5) < 1e-9 && std::abs (out50[1].durationPpq - 0.45) < 1e-9,
              "stretch: 50% halves both each note's offset from phraseStart AND its own duration");
        check (std::abs (out50[3].onsetPpq - 1.5) < 1e-9, "stretch: 50% compresses the whole phrase into half the time");

        auto out200 = odly::applyStretch (notes, 0.0, 200.0f);
        check (std::abs (out200[3].onsetPpq - 6.0) < 1e-9,
              "stretch: 200% doubles the whole phrase's span - the last note now lands twice as far out");

        // Rescale is relative to phraseStartPpq, not absolute ppq zero.
        std::vector<odly::HeldNote> notesAt10 {
            makeNote (10.0, 0.9, 60), makeNote (11.0, 0.9, 62), makeNote (12.0, 0.9, 64), makeNote (13.0, 0.9, 65)
        };
        auto outOffset = odly::applyStretch (notesAt10, 10.0, 50.0f);
        check (std::abs (outOffset[1].onsetPpq - 10.5) < 1e-9,
              "stretch: rescales relative to the phrase's OWN start (10.0 here), not absolute ppq zero");

        // Out-of-range factors are clamped, not left to produce nonsense.
        auto outClampHigh = odly::applyStretch (notes, 0.0, 1000.0f);
        check (std::abs (outClampHigh[3].onsetPpq - (3.0 * 4.0)) < 1e-9,
              "stretch: factor is clamped to the documented [25%,400%] range");
    }

    // --- applyIntervalScale: scales every interval from the phrase's own anchor ---
    {
        std::vector<odly::HeldNote> notes {
            makeNote (0.0, 1.0, 60), makeNote (1.0, 1.0, 64), makeNote (2.0, 1.0, 67), makeNote (3.0, 1.0, 60)
        };
        auto out100 = odly::applyIntervalScale (notes, 100.0f);
        check (out100[1].pitch == 64 && out100[2].pitch == 67, "interval: 100% leaves every pitch unchanged");

        auto out200 = odly::applyIntervalScale (notes, 200.0f);
        check (out200[0].pitch == 60, "interval: the anchor note itself never moves, regardless of factor");
        check (out200[1].pitch == 68, "interval: 200% doubles the leap to note 2 (64 -> anchor+2*4 = 68)");
        check (out200[2].pitch == 74, "interval: 200% doubles the leap to note 3 (67 -> anchor+2*7 = 74)");

        auto out0 = odly::applyIntervalScale (notes, 0.0f);
        check (out0[1].pitch == 60 && out0[2].pitch == 60,
              "interval: 0% collapses every note onto the anchor pitch");

        auto outEmpty = odly::applyIntervalScale ({}, 200.0f);
        check (outEmpty.empty(), "interval: an empty phrase stays empty, no crash");
    }

    // --- applyTransform dispatch ----------------------------------------------
    {
        std::vector<odly::HeldNote> notes {
            makeNote (0.0, 1.0, 60), makeNote (1.0, 1.0, 64), makeNote (2.0, 1.0, 67), makeNote (3.0, 1.0, 60)
        };
        auto same = odly::applyTransform (notes, odly::kTransformNone, 0.0, 4.0, 12, 0, 100.0f, 100.0f, 100.0f);
        check (same[0].pitch == 60, "applyTransform: kTransformNone returns input unchanged");
        auto up = odly::applyTransform (notes, odly::kTransformTranspose, 0.0, 4.0, 12, 0, 100.0f, 100.0f, 100.0f);
        check (up[0].pitch == 72, "applyTransform: dispatches to Transpose correctly");
        auto rot = odly::applyTransform (notes, odly::kTransformRotation, 0.0, 4.0, 12, 1, 100.0f, 100.0f, 100.0f);
        check (rot[0].pitch == 60 && rot[1].pitch == 60, "applyTransform: dispatches to Rotation correctly");
        auto len = odly::applyTransform (notes, odly::kTransformLength, 0.0, 4.0, 12, 0, 50.0f, 100.0f, 100.0f);
        check (len.size() == 2, "applyTransform: dispatches to Length correctly");
        auto m7 = odly::applyTransform (notes, odly::kTransformM7, 0.0, 4.0, 12, 0, 100.0f, 100.0f, 100.0f);
        check (m7[0].pitch == 60, "applyTransform: dispatches to M7 correctly");
        auto stretch = odly::applyTransform (notes, odly::kTransformStretch, 0.0, 4.0, 12, 0, 100.0f, 200.0f, 100.0f);
        check (std::abs (stretch[1].onsetPpq - 2.0) < 1e-9, "applyTransform: dispatches to Stretch correctly");
        auto interval = odly::applyTransform (notes, odly::kTransformInterval, 0.0, 4.0, 12, 0, 100.0f, 100.0f, 200.0f);
        check (interval[1].pitch == 68, "applyTransform: dispatches to Interval correctly");
    }

    // --- proposeTransform: 8-way pick now covers the full vocabulary --------
    {
        int seenKinds[9] = { 0 };   // index 0 unused (kTransformNone never proposed when applyAny)
        bool everyPickInRange = true;
        for (int i = 0; i < 4000; ++i)
        {
            auto p = odly::proposeTransform (99, i, 1.0f);   // restlessness=1 -> always proposes
            if (p.transformKind < odly::kTransformTranspose || p.transformKind > odly::kTransformInterval)
                everyPickInRange = false;
            seenKinds[p.transformKind]++;
        }
        check (everyPickInRange, "proposeTransform: always picks one of the 8 real transforms, never None, at restlessness=1");

        bool allSeen = true;
        for (int k = odly::kTransformTranspose; k <= odly::kTransformInterval; ++k)
            if (seenKinds[k] == 0) allSeen = false;
        check (allSeen, "proposeTransform: all 8 transforms (including the newly-added Interval) get picked across a large sample");
    }

    // --- computePhraseFeatures: cheap shape measurements ----------------------
    {
        std::vector<odly::HeldNote> notes {
            makeNote (0.0, 1.0, 60), makeNote (1.0, 1.0, 64), makeNote (2.0, 1.0, 72)
        };
        auto f = odly::computePhraseFeatures (notes);
        check (f.noteCount == 3, "computePhraseFeatures: counts the notes");
        check (f.pitchSpread == 12, "computePhraseFeatures: pitch spread = max-min (72-60=12)");
        check (std::abs (f.density - 1.5f) < 1e-6f,
              "computePhraseFeatures: density = noteCount / onset span (3 notes / 2 beats = 1.5)");

        auto empty = odly::computePhraseFeatures ({});
        check (empty.noteCount == 0 && empty.pitchSpread == 0, "computePhraseFeatures: an empty phrase is all zeros, no crash");

        std::vector<odly::HeldNote> single { makeNote (0.0, 1.0, 60) };
        auto singleFeatures = odly::computePhraseFeatures (single);
        check (singleFeatures.pitchSpread == 0 && singleFeatures.density > 0.0f,
              "computePhraseFeatures: a single note has zero spread and a floored (not infinite/NaN) density");
    }

    // --- proposeWeightedTransform: bias direction matches the phrase's content ---
    {
        const odly::PhraseFeatures dense { 8, 4, 8.0f };     // many notes, tight span, fast
        const odly::PhraseFeatures sparse { 8, 4, 0.25f };   // same note count/range, spread thin

        int denseLengthCount = 0, sparseLengthCount = 0;
        int denseStretchCount = 0, sparseStretchCount = 0;
        for (int i = 0; i < 4000; ++i)
        {
            const auto d = odly::proposeWeightedTransform (42, i, 1.0f, dense);
            const auto s = odly::proposeWeightedTransform (42, i, 1.0f, sparse);
            if (d.transformKind == odly::kTransformLength) ++denseLengthCount;
            if (s.transformKind == odly::kTransformLength) ++sparseLengthCount;
            if (d.transformKind == odly::kTransformStretch) ++denseStretchCount;
            if (s.transformKind == odly::kTransformStretch) ++sparseStretchCount;
        }
        check (denseLengthCount > sparseLengthCount,
              "proposeWeightedTransform: a DENSE phrase picks Length more often than a sparse one with the same note count/range");
        check (sparseStretchCount > denseStretchCount,
              "proposeWeightedTransform: a SPARSE phrase picks Stretch more often than a dense one with the same note count/range");

        const odly::PhraseFeatures narrow { 8, 1, 2.0f };   // same count/density, tight pitch range
        const odly::PhraseFeatures wide { 8, 24, 2.0f };    // same count/density, wide pitch range
        int narrowInversionCount = 0, wideInversionCount = 0;
        for (int i = 0; i < 4000; ++i)
        {
            const auto n = odly::proposeWeightedTransform (42, i, 1.0f, narrow);
            const auto w = odly::proposeWeightedTransform (42, i, 1.0f, wide);
            if (n.transformKind == odly::kTransformInversion) ++narrowInversionCount;
            if (w.transformKind == odly::kTransformInversion) ++wideInversionCount;
        }
        check (wideInversionCount > narrowInversionCount,
              "proposeWeightedTransform: a WIDE-range phrase picks Inversion more often than a narrow one with the same count/density");

        const auto a = odly::proposeWeightedTransform (42, 7, 0.5f, dense);
        const auto b = odly::proposeWeightedTransform (42, 7, 0.5f, dense);
        check (a.applyAny == b.applyAny && a.transformKind == b.transformKind,
              "proposeWeightedTransform: identical inputs always produce identical outputs (determinism)");

        check (! odly::proposeWeightedTransform (42, 7, 0.0f, dense).applyAny,
              "proposeWeightedTransform: restlessness=0 still never proposes a transform (the gate is unchanged)");
    }

    // --- resolveRandomTransposeSemitones: deterministic, ranged, symmetric ---
    {
        const int a = odly::resolveRandomTransposeSemitones (5, 3, 12);
        const int b = odly::resolveRandomTransposeSemitones (5, 3, 12);
        check (a == b, "resolveRandomTransposeSemitones: identical inputs always produce identical outputs (determinism)");
        check (a >= -12 && a <= 12, "resolveRandomTransposeSemitones: result stays within [-range, +range]");

        check (odly::resolveRandomTransposeSemitones (5, 3, 0) == 0,
              "resolveRandomTransposeSemitones: a zero range always resolves to 0");

        bool sawNegative = false, sawPositive = false;
        for (int i = 0; i < 200; ++i)
        {
            const int v = odly::resolveRandomTransposeSemitones (5, i, 12);
            if (v < 0) sawNegative = true;
            if (v > 0) sawPositive = true;
        }
        check (sawNegative && sawPositive, "resolveRandomTransposeSemitones: draws both negative and positive values across a sample");
    }

    // --- resolveRandomRotationSteps: same shape as Transpose's, salt 4 ------
    {
        const int a = odly::resolveRandomRotationSteps (5, 3, 8);
        const int b = odly::resolveRandomRotationSteps (5, 3, 8);
        check (a == b, "resolveRandomRotationSteps: identical inputs always produce identical outputs (determinism)");
        check (a >= -8 && a <= 8, "resolveRandomRotationSteps: result stays within [-range, +range]");
        check (odly::resolveRandomRotationSteps (5, 3, 0) == 0,
              "resolveRandomRotationSteps: a zero range always resolves to 0");

        bool sawNegative = false, sawPositive = false;
        for (int i = 0; i < 200; ++i)
        {
            const int v = odly::resolveRandomRotationSteps (5, i, 8);
            if (v < 0) sawNegative = true;
            if (v > 0) sawPositive = true;
        }
        check (sawNegative && sawPositive, "resolveRandomRotationSteps: draws both negative and positive values across a sample");
    }

    // --- resolveRandomLengthPercent: a CEILING bound, not symmetric ---------
    {
        const float a = odly::resolveRandomLengthPercent (5, 3, 80.0f);
        const float b = odly::resolveRandomLengthPercent (5, 3, 80.0f);
        check (std::abs (a - b) < 1e-6f, "resolveRandomLengthPercent: identical inputs always produce identical outputs (determinism)");
        check (a >= 1.0f && a <= 80.0f, "resolveRandomLengthPercent: result stays within [1, ceiling]");
        check (std::abs (odly::resolveRandomLengthPercent (5, 3, 1.0f) - 1.0f) < 1e-6f,
              "resolveRandomLengthPercent: a ceiling of 1% always resolves to exactly 1%");

        bool sawLow = false, sawHigh = false;
        for (int i = 0; i < 200; ++i)
        {
            const float v = odly::resolveRandomLengthPercent (5, i, 100.0f);
            if (v < 50.0f) sawLow = true;
            if (v > 50.0f) sawHigh = true;
        }
        check (sawLow && sawHigh, "resolveRandomLengthPercent: spreads across the full range, not clustered at one end");
    }

    // --- resolveRandomStretchPercent: neutral point is 100%, not 0 ----------
    {
        const float a = odly::resolveRandomStretchPercent (5, 3, 200.0f);
        const float b = odly::resolveRandomStretchPercent (5, 3, 200.0f);
        check (std::abs (a - b) < 1e-6f, "resolveRandomStretchPercent: identical inputs always produce identical outputs (determinism)");
        check (a >= 100.0f && a <= 200.0f, "resolveRandomStretchPercent: a bound ABOVE 100 draws only slower/longer values, never below 100");

        const float c = odly::resolveRandomStretchPercent (5, 3, 50.0f);
        check (c >= 50.0f && c <= 100.0f, "resolveRandomStretchPercent: a bound BELOW 100 draws only faster/shorter values, never above 100");

        check (std::abs (odly::resolveRandomStretchPercent (5, 3, 100.0f) - 100.0f) < 1e-6f,
              "resolveRandomStretchPercent: a bound of exactly 100% always resolves to 100% (no range)");
    }

    // --- resolveRandomIntervalPercent: same shape as Stretch's, salt 9 ------
    {
        const float a = odly::resolveRandomIntervalPercent (5, 3, 200.0f);
        const float b = odly::resolveRandomIntervalPercent (5, 3, 200.0f);
        check (std::abs (a - b) < 1e-6f, "resolveRandomIntervalPercent: identical inputs always produce identical outputs (determinism)");
        check (a >= 100.0f && a <= 200.0f, "resolveRandomIntervalPercent: a bound ABOVE 100 draws only wider values, never below 100");

        const float c = odly::resolveRandomIntervalPercent (5, 3, 50.0f);
        check (c >= 50.0f && c <= 100.0f, "resolveRandomIntervalPercent: a bound BELOW 100 draws only narrower values, never above 100");

        check (std::abs (odly::resolveRandomIntervalPercent (5, 3, 100.0f) - 100.0f) < 1e-6f,
              "resolveRandomIntervalPercent: a bound of exactly 100% always resolves to 100% (no range)");
    }

    // --- Quantized Stretch: a fixed notation-friendly ratio vocabulary ------
    {
        const auto& ratios = odly::quantizedStretchRatios();
        check (ratios.size() >= 9, "quantizedStretchRatios: has a real vocabulary, not a token list");
        bool has100 = false;
        for (float r : ratios) if (std::abs (r - 100.0f) < 1e-6f) has100 = true;
        check (has100, "quantizedStretchRatios: includes 100% (unchanged)");
    }
    {
        check (std::abs (odly::snapToQuantizedStretch (100.0f) - 100.0f) < 1e-6f,
              "snapToQuantizedStretch: an exact legal value snaps to itself");
        check (std::abs (odly::snapToQuantizedStretch (105.0f) - 100.0f) < 1e-6f,
              "snapToQuantizedStretch: 105% (close to 100) snaps to 100%");
        check (std::abs (odly::snapToQuantizedStretch (30.0f) - (100.0f / 3.0f)) < 1e-3f,
              "snapToQuantizedStretch: 30% snaps to the nearer of 25% and 33.3% (33.3%)");
        check (std::abs (odly::snapToQuantizedStretch (180.0f) - 200.0f) < 1e-6f,
              "snapToQuantizedStretch: 180% is closer to 200% than 150%, snaps to 200%");
    }
    {
        const float a = odly::resolveRandomQuantizedStretchPercent (5, 3, 200.0f);
        const float b = odly::resolveRandomQuantizedStretchPercent (5, 3, 200.0f);
        check (std::abs (a - b) < 1e-6f, "resolveRandomQuantizedStretchPercent: identical inputs always produce identical outputs (determinism)");

        bool aIsLegal = false;
        for (float r : odly::quantizedStretchRatios()) if (std::abs (a - r) < 1e-3f) aIsLegal = true;
        check (aIsLegal, "resolveRandomQuantizedStretchPercent: the result is always an EXACT legal ratio, never an in-between value");
        check (a >= 100.0f - 1e-3f && a <= 200.0f + 1e-3f,
              "resolveRandomQuantizedStretchPercent: stays within [100, bound] for a bound above 100");

        const float c = odly::resolveRandomQuantizedStretchPercent (5, 3, 50.0f);
        check (c >= 50.0f - 1e-3f && c <= 100.0f + 1e-3f,
              "resolveRandomQuantizedStretchPercent: stays within [bound, 100] for a bound below 100");

        check (std::abs (odly::resolveRandomQuantizedStretchPercent (5, 3, 100.0f) - 100.0f) < 1e-6f,
              "resolveRandomQuantizedStretchPercent: a bound of exactly 100% always resolves to 100%");

        int distinctValues = 0;
        float seenA = -1.0f, seenB = -1.0f;
        for (int i = 0; i < 100 && distinctValues < 2; ++i)
        {
            const float v = odly::resolveRandomQuantizedStretchPercent (5, i, 400.0f);
            if (seenA < 0.0f) seenA = v;
            else if (std::abs (v - seenA) > 1e-3f) { seenB = v; distinctValues = 2; }
        }
        check (distinctValues == 2, "resolveRandomQuantizedStretchPercent: draws more than one distinct legal ratio across a sample");
    }

    // --- shiftOutputNotes: used by Overlap Mode "Wait" to release a queued answer ---
    {
        odly::Phrase p;
        p.phraseStartPpq = 0.0;
        p.phraseEndPpq = 2.0;
        p.scheduledFirePpq = 8.0;
        odly::ScheduledNote a; a.outputOnsetPpq = 8.0; a.outputOffPpq = 8.9;
        odly::ScheduledNote b; b.outputOnsetPpq = 9.0; b.outputOffPpq = 9.9;
        p.outputNotes = { a, b };

        odly::shiftOutputNotes (p, 5.0);
        check (std::abs (p.outputNotes[0].outputOnsetPpq - 13.0) < 1e-9 && std::abs (p.outputNotes[0].outputOffPpq - 13.9) < 1e-9,
              "shiftOutputNotes: shifts the first note's onset AND off by the same amount");
        check (std::abs (p.outputNotes[1].outputOnsetPpq - 14.0) < 1e-9 && std::abs (p.outputNotes[1].outputOffPpq - 14.9) < 1e-9,
              "shiftOutputNotes: shifts every note, preserving the phrase's own internal spacing (1 beat apart, still)");
    }

    // --- resolveRandomHoldBars: [1, ceiling], never 0 -------------------------
    {
        const int a = odly::resolveRandomHoldBars (5, 3, 8);
        const int b = odly::resolveRandomHoldBars (5, 3, 8);
        check (a == b, "resolveRandomHoldBars: identical inputs always produce identical outputs (determinism)");
        check (a >= 1 && a <= 8, "resolveRandomHoldBars: result stays within [1, ceiling]");
        check (odly::resolveRandomHoldBars (5, 3, 1) == 1, "resolveRandomHoldBars: a ceiling of 1 always resolves to 1");

        bool sawLow = false, sawHigh = false, everZero = false;
        for (int i = 0; i < 100; ++i)
        {
            const int v = odly::resolveRandomHoldBars (5, i, 16);
            if (v <= 4) sawLow = true;
            if (v >= 13) sawHigh = true;
            if (v < 1) everZero = true;
        }
        check (! everZero, "resolveRandomHoldBars: NEVER draws 0 across a sample - Hold Bars=0 is a dedicated pause state, not a random outcome");
        check (sawLow && sawHigh, "resolveRandomHoldBars: spreads across the full range, not clustered at one end");
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
