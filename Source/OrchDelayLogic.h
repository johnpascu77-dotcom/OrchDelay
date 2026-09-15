#pragma once

#include <vector>
#include <juce_audio_basics/juce_audio_basics.h>

// Pure hold-and-refire logic for OrchDelay. No plugin host, no GUI, no real
// transport - the processor owns all of that (polling AudioPlayHead, sample-
// accurate MidiBuffer emission) and hands ppq-timestamped note data to this
// file. This is the part OrchDelayLogicCheck exercises directly.
//
// Scope (see Docs/OrchDelay_Design.md SS2): this holds a captured phrase and
// fires it back once, N bars later, optionally transformed as a whole. It
// does not loop/repeat a phrase, and it does not talk CC-to-MPL - the full
// transform vocabulary (Transpose/Retrograde/Inversion/Rotation/Length/M7,
// see Docs SS14 for the v1.1 expansion) is reimplemented here directly on
// the buffered note data, not routed through an external engine.
namespace odly
{
    // One buffered note occurrence. `seq` identifies THIS specific occurrence
    // (never (channel,pitch) alone) - OrchPiano's own devlog documents the
    // identical bug class hit three separate times from note-off/restrike
    // matching by (channel,pitch) tuple alone, which collides on a repeated
    // pitch or an overlapping re-attack. Assign seq from one monotonic
    // counter the instant a note-on is captured.
    struct HeldNote
    {
        int channel = 1;              // 1-16, as received
        int pitch = 60;                // 0-127
        int velocity = 100;            // 1-127, original note-on velocity
        double onsetPpq = 0.0;         // absolute ppq of the original note-on
        double durationPpq = 0.0;      // 0 until the matching note-off arrives (see closePhrase)
        juce::int64 seq = -1;
        int phraseId = -1;
        bool hasNoteOff = false;
    };

    // One transformed, schedule-resolved note ready to be fired
    // INDEPENDENTLY, whenever its own outputOnsetPpq arrives - never bundled
    // with the rest of its phrase into a single block's emission. Built once
    // by buildOutputNotes() at the moment a phrase is scheduled; firing later
    // only ever checks each note's own outputOnsetPpq against the current
    // block, never re-derives the transform.
    struct ScheduledNote
    {
        int channel = 1;
        int pitch = 60;
        int velocity = 100;
        double outputOnsetPpq = 0.0;   // absolute ppq this note actually sounds at
        double outputOffPpq = 0.0;     // absolute ppq its note-off sounds at
        juce::int64 seq = -1;
        bool emitted = false;
    };

    // A silence-gap-bounded group of notes, captured and fired as one whole
    // unit rather than note-by-note - preserves the "syncopated intention"
    // of the original performance the same way a musical phrase, not an
    // isolated pitch, is the natural unit for a call-and-response device.
    // "Fired as one whole unit" describes CAPTURE (buffered together, held
    // together, transformed together) - firing itself still happens NOTE BY
    // NOTE, each at its own independently-arriving output time (see
    // ScheduledNote/outputNotes below); bundling every note of a multi-note
    // phrase into one block's emission was a real, previously-hidden bug
    // that collapsed the echoed phrase's own rhythm into a simultaneous
    // cluster - only exposed once phrases could actually contain more than
    // one note (see this repo's own devlog).
    struct Phrase
    {
        int phraseId = -1;
        std::vector<HeldNote> notes;      // always in original onset order
        double phraseStartPpq = 0.0;
        double phraseEndPpq = 0.0;
        bool closed = false;
        double scheduledFirePpq = -1.0;   // capture-time ppq + N bars, frozen at closure - see computeScheduledFirePpq
        int chosenTransform = 0;          // TransformKind, resolved once at closure
        bool fired = false;
        std::vector<ScheduledNote> outputNotes;   // built once by buildOutputNotes() at schedule time
    };

    enum TransformKind
    {
        kTransformNone = 0,
        kTransformTranspose = 1,
        kTransformRetrograde = 2,
        kTransformInversion = 3,
        kTransformRotation = 4,
        kTransformLength = 5,
        kTransformM7 = 6,
        kTransformStretch = 7,
        kTransformInterval = 8
    };

    // How a due answer behaves when an earlier answer is still audibly
    // sounding - see Docs SS17. Not every instrument downstream is
    // polyphonic (most of an orchestra isn't), so "just let them overlap"
    // isn't always the right default, even though it's genuinely useful
    // for piano-like material.
    enum OverlapMode
    {
        kOverlapOverlap = 0,   // fire on schedule regardless - today's original behavior
        kOverlapWait = 1,      // hold until the previous answer finishes, then play in full, own rhythm preserved
        kOverlapSkip = 2       // if still busy when due, discard this answer entirely - never plays
    };

    // A note-on/note-off/note-off-with-no-velocity event, the raw input this
    // logic groups into phrases. Mirrors just enough of juce::MidiMessage's
    // shape to stay host/JUCE-free at the call boundary while still being
    // trivial for the processor to build from a real MidiBuffer iteration.
    struct RawMidiEvent
    {
        bool isNoteOn = false;   // false = note-off (or note-on velocity 0)
        int channel = 1;
        int pitch = 60;
        int velocity = 0;
        double ppq = 0.0;
    };

    struct CaptureResult
    {
        bool phraseClosed = false;   // true if this event's gap closed openPhrase
        Phrase closedPhrase;         // valid only when phraseClosed - a COPY, ready to schedule/fire
    };

    // Feeds one raw event into `openPhrase`, the single in-progress phrase
    // currently accepting new notes (starts as a default-constructed empty
    // Phrase, phraseId == -1). A note-on either extends `openPhrase` or, if
    // the REST since the previous note's END is at/above `phraseGapBeats`,
    // closes it first (via closePhrase(), using `holdBars`/`beatsPerBarNow`)
    // and starts a fresh one. Gap is measured from the previous note's END
    // (onset + duration), NOT onset-to-onset - two notes played legato, with
    // durations close to their own spacing, must not read as a phrase
    // boundary just because their onsets are far apart (onset-to-onset was
    // the real bug behind "4 played notes, only 3 echoed back": ordinary
    // quarter-note-spaced playing at a ~1-beat gap setting fractured into
    // one phrase per note). A previous note still sounding (no note-off yet)
    // has zero rest by definition - can't be "past" a gap that hasn't ended.
    // A note-off is matched FIFO against the oldest still-open
    // (hasNoteOff==false) HeldNote in `openPhrase` sharing (channel,pitch) -
    // a stray note-off with no match (including one for a note whose phrase
    // already closed, since closePhrase() marks every note hasNoteOff==true
    // on the way out) is dropped silently, matching house style for edge
    // MIDI throughout this ecosystem.
    //
    // `nextNoteSeq` and `nextPhraseId` are counters owned by the caller (the
    // processor), threaded through by reference so this stays a pure
    // function with no hidden static state.
    CaptureResult captureEvent (const RawMidiEvent& event, double phraseGapBeats,
                               int holdBars, double beatsPerBarNow,
                               juce::int64& nextNoteSeq, int& nextPhraseId,
                               Phrase& openPhrase);

    // Closes `phrase` (which must currently be open/unclosed): gives every
    // note still missing a note-off a fixed 0.5-beat fallback duration and
    // marks it hasNoteOff=true (see Docs SS2, "Notes with no note-off at
    // closure time" - a stuck key must never hang closure indefinitely, and
    // marking it closed here is what makes a later real note-off for it
    // correctly read as a stray with no open match, per captureEvent's own
    // doc), finalizes phraseStartPpq/phraseEndPpq, and computes
    // scheduledFirePpq = phraseEndPpq + holdBars * beatsPerBar, floored at
    // `nowPpq` (the ppq at the moment of THIS closure call) - anchored to
    // where the phrase ENDS, not where it started (the natural reading of
    // "responsorial": answer N bars after the phrase is done, not N bars
    // after it began), and never allowed to land before the phrase is even
    // known to be closed. Without the `nowPpq` floor, a short holdBars
    // (comparable to or shorter than phraseGapBeats) could put
    // phraseEndPpq + holdBars*bar BEFORE the phrase actually finishes
    // closing - since closure itself can't happen until phraseGapBeats of
    // rest has elapsed past phraseEndPpq - which crammed the echo's first
    // note or two into whichever block first noticed the (already overdue)
    // phrase, a real live-tested bug. Uses the METER ACTIVE AT THIS CALL
    // (capture time), never re-derived later - see Docs SS3 for why
    // fire-time meter changes must not retroactively change what "N bars"
    // meant. Sets `phrase.closed = true`.
    void closePhrase (Phrase& phrase, int holdBars, double beatsPerBar, double nowPpq);

    // Proactively closes `openPhrase` if the rest since its last note's END
    // is at/above `phraseGapBeats`, with NO new note-on required to detect
    // it - call once per block, after capturing that block's events, while
    // the transport is playing. Without this, a phrase whose last note is
    // never followed by anything else (a clip that ends, or the last take
    // before the player stops) would sit open forever and get silently
    // discarded on stop instead of firing back - the real cause behind
    // "responds only to live input": live playing naturally keeps feeding
    // new notes that close out trailing phrases via captureEvent's own gap
    // check, but a finite clip's last phrase has no such follow-up note.
    // No-ops (phraseClosed stays false) if `openPhrase` is empty or its last
    // note has no note-off yet (still sounding - can't time out something
    // that hasn't ended). Uses the same closePhrase() as captureEvent, so
    // scheduledFirePpq math is identical either way.
    CaptureResult checkPhraseTimeout (double nowPpq, double phraseGapBeats,
                                      int holdBars, double beatsPerBarNow,
                                      Phrase& openPhrase);

    // --- Stuck-note-cleanup discipline (see Docs SS2) -----------------------
    // A single note-off "occurrence" this OrchDelay instance owes the output
    // for something it fired earlier, tracked by seq so an overlapping
    // same-pitch re-fire can never cross-release the wrong one.
    struct ActiveFiredNote
    {
        int channel = 1;
        int pitch = 60;
        juce::int64 seq = -1;
        double noteOffPpq = 0.0;
    };

    // How live input relates to a currently-sounding echo (see Docs SS22).
    // Reopens the original "live notes are always fully swallowed" decision
    // (Docs SS2/SS3.2) - kept as the default (kCaptureReplace), since it's
    // still exactly right for a clean call-and-response device, but not
    // every use wants that.
    enum CaptureMode
    {
        kCaptureReplace = 0,   // original behavior - live notes always fully swallowed, only the echo sounds
        kCaptureOverlay = 1,   // live notes always pass through immediately, alongside any echo
        kCaptureDuck = 2       // live notes pass through only OUTSIDE the currently-sounding echo's own pitch range
    };

    // True if `pitch` falls outside the [min,max] pitch range currently
    // spanned by `active` (the notes OrchDelay has actually fired and is
    // still sounding) - the decision Duck capture mode uses per live note.
    // An empty `active` list means nothing is currently occupying any
    // register, so everything counts as "outside" (pass through freely).
    bool isOutsideActiveRange (const std::vector<ActiveFiredNote>& active, int pitch);

    // Bar length in ppq (quarter notes) for a given time signature - a
    // quarter note is always 1.0 ppq by definition, so a bar is
    // numerator * (4.0 / denominator) quarter notes.
    double beatsPerBar (int numerator, int denominator);

    // --- Transform vocabulary (v1: Transpose, Retrograde, Inversion) -------
    // Each takes a phrase's own note list and returns a NEW list (never
    // mutates the stored original - the same phrase could in principle be
    // inspected again before it fires).

    std::vector<HeldNote> applyTranspose (const std::vector<HeldNote>& notes, int semitones);

    // Pitch-axis mirror around the phrase's OWN first/anchor note, not MPL's
    // fixed axis=60 (middle C) - a deliberate divergence, see Docs SS4:
    // MPL's fixed axis suits a visible on-screen step pattern; OrchDelay
    // processes arbitrary live-played material in any register, and a fixed
    // axis would throw a low phrase into a wildly different, possibly
    // unplayable register.
    std::vector<HeldNote> applyInversion (const std::vector<HeldNote>& notes);

    // Whole-phrase time reversal: the note that was captured LAST now plays
    // FIRST. Reflects each note's onset around the phrase's own midpoint
    // (phraseStart + phraseEnd - (onset + duration)), preserving each note's
    // own duration - the natural generalization of MPL's own Retrograde
    // (which reverses read POSITION within a fixed step grid, meaningless
    // for an arbitrary-length real-timestamp phrase) to a continuous-
    // timestamp domain. See Docs SS4.
    std::vector<HeldNote> applyRetrograde (const std::vector<HeldNote>& notes,
                                           double phraseStartPpq, double phraseEndPpq);

    // Cyclic reassignment of WHICH captured note's pitch/velocity/channel
    // sounds at each onset/duration "slot" - a direct port of MPL's own
    // Rotation (`sourceStepIndex = playbackStepIndex - rotation`, wrapped
    // mod the pattern length), generalized from a fixed 16-step grid to
    // OrchDelay's own variable-length note list: slot i takes its pitch/
    // velocity/channel from slot `(i - steps) mod noteCount`, while keeping
    // slot i's OWN onset/duration (the phrase's rhythmic skeleton is
    // unchanged - only which pitch lands where shifts). `steps` wraps
    // automatically for any phrase length, including a 0/1-note phrase
    // (a no-op, same as MPL rotating a 1-step loop).
    std::vector<HeldNote> applyRotation (const std::vector<HeldNote>& notes, int steps);

    // Truncates the phrase to its first `lengthPercent`% of notes (by onset
    // order), at least 1 - a direct generalization of MPL's own Length (how
    // many of the pattern's steps play before it loops) to a one-shot
    // device with no loop to clamp: here it simply trims how much of the
    // captured material gets echoed at all.
    std::vector<HeldNote> applyLength (const std::vector<HeldNote>& notes, float lengthPercent);

    // Pitch-class multiplication by 7 mod 12 - ported byte-for-byte from
    // MPL's own M7 (`pitchClass' = (pitchClass * 7) mod 12`, octave
    // register left alone). A genuine twelve-tone permutation, distinct
    // from Transpose (additive) and Inversion (reflective); needs no domain
    // adaptation since it's a pure per-note pitch operation with no timing/
    // grid dependency - the one v1.1 transform that ports over unchanged.
    std::vector<HeldNote> applyM7 (const std::vector<HeldNote>& notes);

    // Scales every note's own interval from the phrase's OWN anchor note
    // (its first/anchor note, same convention as Inversion) by
    // `scalePercent/100` - 100% = unchanged, >100% widens the melodic
    // shape's leaps (the same contour, reaching further), <100% narrows it
    // (converging toward the anchor as the percent approaches 0, where
    // every note collapses onto the anchor pitch itself). Distinct from
    // Transpose (shifts the whole phrase together, shape unchanged) and
    // Inversion (mirrors the shape, exact interval sizes preserved) - this
    // is the one transform that changes the SIZE of the melodic shape while
    // keeping its up/down silhouette recognizable. No MPL/transcript
    // precedent - the user's own idea, surfaced directly.
    std::vector<HeldNote> applyIntervalScale (const std::vector<HeldNote>& notes, float scalePercent);

    // Proportional time-stretch: rescales every note's onset (relative to
    // the phrase's OWN start) and duration by `stretchPercent/100` -
    // 100% = unchanged, <100% = the echo plays back faster/shorter,
    // >100% = slower/longer, while every note's RELATIVE rhythmic position
    // within the phrase is preserved exactly (a straight proportional
    // rescale, not a per-note offset). No precedent in MPL to port from -
    // MPL's own patterns are locked to their host's fixed BPM/step grid,
    // with nothing to stretch; this only exists because OrchDelay owns real
    // timestamped material it can freely rescale before re-emitting it.
    // Composes naturally with the existing schedule-time math in
    // buildOutputNotes, which only ever looks at each note's offset from
    // phraseStartPpq - a stretched offset lands correctly with no special
    // casing needed there.
    std::vector<HeldNote> applyStretch (const std::vector<HeldNote>& notes, double phraseStartPpq,
                                        float stretchPercent);

    // Dispatches to the right transform above by `TransformKind`; kTransformNone
    // returns the input unchanged (a plain copy).
    std::vector<HeldNote> applyTransform (const std::vector<HeldNote>& notes, int transformKind,
                                          double phraseStartPpq, double phraseEndPpq, int transposeSemitones,
                                          int rotationSteps, float lengthPercent, float stretchPercent,
                                          float intervalScalePercent);

    // Resolves `phrase`'s chosen transform AND the schedule-time shift
    // (scheduledFirePpq relative to phraseStartPpq) into a list of
    // INDEPENDENTLY-fireable output notes, computed ONCE - each note's
    // outputOnsetPpq/outputOffPpq is a final, absolute ppq position. Call
    // this once when a phrase is scheduled (right after chosenTransform is
    // resolved); firing later just checks each note's own outputOnsetPpq
    // against the current block, one at a time - never bundles a whole
    // phrase's notes into a single block's emission (see Phrase's own doc
    // comment for why that used to be a real bug).
    std::vector<ScheduledNote> buildOutputNotes (const Phrase& phrase, int transposeSemitones,
                                                 int rotationSteps, float lengthPercent, float stretchPercent,
                                                 float intervalScalePercent);

    // Shifts EVERY note in phrase.outputNotes forward by `shiftPpq` (adds it
    // to both outputOnsetPpq and outputOffPpq) - used by Overlap Mode
    // "Wait" (see Docs SS17): when a phrase's own first note was due but
    // the device was still busy playing an earlier answer, this preserves
    // the phrase's own internal rhythm once it's finally released to start,
    // rather than firing every already-overdue note in a single clump the
    // moment the coast clears. Call exactly once per phrase, right when
    // it's released - never mutates already-emitted notes' meaning, since
    // release only ever happens before a phrase's first note has fired.
    void shiftOutputNotes (Phrase& phrase, double shiftPpq);

    // --- Restlessness-driven transform proposal -----------------------------
    // A stateless FNV-1a-style hash (byte-for-byte port of OrchGate's own
    // resolveResponseOverlay hash lambda, OrchGateProcessor.cpp ~line 580) -
    // no RNG object, no stored seed, fully deterministic and reload-stable:
    // same (instanceSeed, phraseCounter, salt) always hashes to the same
    // value. See Docs SS5 for why this + OrchConductor's threshold-a-choice
    // idiom (not ComposerMastermind's continuous-jitter one) were chosen.
    juce::uint32 fnv1aHash (int a, int b, int salt);

    // Maps a hash to a [0,1) float, same construction as OrchGate's own unitFor.
    float hashUnit (int a, int b, int salt);

    struct TransformProposal
    {
        bool applyAny = false;
        int transformKind = kTransformNone;
    };

    // restlessness (0..1) IS the probability of proposing a non-verbatim
    // transform for this phrase - at 0, always verbatim; at 1, always
    // transforms. If applyAny, an equal-weight 8-way pick among the full
    // transform vocabulary - Transpose/Retrograde/Inversion/Rotation/
    // Length/M7/Stretch/Interval (no evidence yet to favor one - see Docs
    // SS7; extended 3-way -> 6-way -> 7-way -> 8-way as Rotation/Length/M7,
    // then Stretch, then Interval, were added).
    // `instanceSeed`: this OrchDelay instance's own identity (0-127 param).
    // `phraseCounter`: increments once per phrase closure - takes the role
    // OrchGate's broadcast "mode" CC plays, since v1 has no OrchConductor
    // bridge to hash against.
    TransformProposal proposeTransform (int instanceSeed, int phraseCounter, float restlessness);

    // --- Content-aware transform weighting (see Docs SS19) -------------------
    // A phrase's own basic shape, cheap to compute from its captured notes -
    // feeds proposeWeightedTransform's bias below. Never mutates or judges
    // the phrase itself, just measures it.
    struct PhraseFeatures
    {
        int noteCount = 0;
        int pitchSpread = 0;   // max pitch - min pitch, semitones
        float density = 0.0f;  // notes per beat (noteCount / the phrase's own onset span)
    };

    PhraseFeatures computePhraseFeatures (const std::vector<HeldNote>& notes);

    // Same "should we transform at all" gate as proposeTransform
    // (restlessness), but if triggered, WHICH transform is picked is biased
    // by the phrase's own features instead of a flat 1-in-8 chance:
    // Inversion and Interval favor a wide pitch spread (more dramatic to
    // mirror/scale a shape that already spans a lot); Rotation favors a
    // higher note count (needs several notes to be interesting); Length
    // favors high density (more to meaningfully trim from a busy phrase);
    // Stretch favors low density/sparse material (more room and time to
    // stretch into). Transpose, Retrograde, and M7 stay at a flat baseline
    // weight - each is "always reasonable" regardless of content. Still
    // fully deterministic (same instanceSeed+phraseCounter+features always
    // picks the same transform).
    TransformProposal proposeWeightedTransform (int instanceSeed, int phraseCounter, float restlessness,
                                                const PhraseFeatures& features);

    // --- Phrase-quality gate (see Docs SS20) ----------------------------------
    // A 0..1 "worth answering" score for a captured phrase, from cheap,
    // purely structural measurements - never a judgment about musical
    // TASTE, just shape: a single note, or several notes all on the same
    // repeated pitch, score near 0; a phrase with several DISTINCT pitches
    // spanning a real melodic range scores near 1. Equal-weighted average
    // of 3 factors: note count (ramps to 1.0 by ~6 notes - doesn't take
    // much to feel "worth it"), pitch variety (distinct pitches / total
    // notes - a repeated single pitch scores near 0 regardless of note
    // count), and pitch spread (saturates at an octave). A phrase below the
    // Minimum Interest threshold (default 0% = gate disabled) is captured
    // and closed normally but never scheduled to echo at all - see
    // OrchDelayProcessor's own scheduleClosedPhrase.
    float computePhraseInterest (const std::vector<HeldNote>& notes);

    // --- Multi-motive memory bank (see Docs SS21) -----------------------------
    // Just enough of a PAST phrase's own captured shape to echo it again
    // later: its notes plus the phraseStart/End that transforms referencing
    // the phrase's own timing (Retrograde, Stretch) need. The processor owns
    // a small pool of these (most-recent-N actually played, not echoed -
    // see OrchDelayProcessor's own phraseMemory).
    struct MemoryEntry
    {
        std::vector<HeldNote> notes;
        double phraseStartPpq = 0.0;
        double phraseEndPpq = 0.0;
    };

    struct MemoryCallbackDecision
    {
        bool useCallback = false;
        int poolIndex = -1;   // valid only when useCallback is true
    };

    // Resolves whether a just-closed phrase should be replaced by an OLDER
    // one from the memory pool instead of echoing itself, and if so, which
    // pool index (uniform among the whole pool - no "most recent"/"highest-
    // energy" weighting in this first version, see Docs SS21's own scope
    // note). `poolSize` is the memory pool's CURRENT size, not including the
    // phrase currently being closed (the caller adds it to the pool only
    // AFTER this decision, so a phrase can never call back to itself).
    // Deterministic like every other seeded choice here - salt 11 for the
    // gate, salt 12 for which index. Returns useCallback=false immediately
    // if poolSize<=0 (nothing to reach back to yet).
    MemoryCallbackDecision resolveMemoryCallback (int instanceSeed, int phraseCounter,
                                                  float callbackProbabilityPercent, int poolSize);

    // Resolves the ACTUAL transpose amount to use when Random Transpose mode
    // is on: deterministic (same instanceSeed+phraseCounter -> same result,
    // reload-stable, same hash construction as proposeTransform but salt 3
    // to stay independent of the transform-choice draws), drawn uniformly
    // from [-|rangeSemitones|, +|rangeSemitones|]. The Transpose (semitones)
    // parameter itself becomes this symmetric RANGE bound in Random mode,
    // rather than a literal fixed amount - so raising/lowering that one
    // slider still controls "how far," just now as a ceiling instead of an
    // exact value. rangeSemitones==0 always resolves to 0 (no range to draw
    // from).
    int resolveRandomTransposeSemitones (int instanceSeed, int phraseCounter, int rangeSemitones);

    // Same idea as resolveRandomTransposeSemitones, for Rotation - the
    // Rotation (steps) slider becomes a symmetric range bound
    // [-|rangeSteps|, +|rangeSteps|] when Random Rotation is on. Salt 4,
    // independent of every other seeded draw in this file.
    int resolveRandomRotationSteps (int instanceSeed, int phraseCounter, int rangeSteps);

    // Same idea again, for Length - but Length has no negative/symmetric
    // meaning (it's always a 0-100% fraction of the phrase), so the Length
    // (%) slider becomes a CEILING instead: drawn uniformly from
    // [1, ceilingPercent]. Salt 5.
    float resolveRandomLengthPercent (int instanceSeed, int phraseCounter, float ceilingPercent);

    // Stretch's neutral point is 100% (unchanged), not 0 - so unlike
    // Transpose/Rotation's symmetric-around-0 bound, this draws uniformly
    // between 100% and `boundPercent`, whichever side of 100 that bound
    // falls on setting the direction (a bound of 200% wanders slower/
    // longer only, never faster; a bound of 50% wanders faster/shorter
    // only). boundPercent==100 always resolves to exactly 100 (no range).
    // Salt 6.
    float resolveRandomStretchPercent (int instanceSeed, int phraseCounter, float boundPercent);

    // Same idea, for Hold Bars: drawn uniformly from [1, ceilingBars]
    // (never 0 - Hold Bars=0 is the dedicated "pause capturing" state, see
    // Docs SS17, and a random draw should never silently re-enable
    // capturing by chance). ceilingBars is clamped to [1,16]; a ceiling of
    // 1 always resolves to 1 (no range). Salt 8.
    int resolveRandomHoldBars (int instanceSeed, int phraseCounter, int ceilingBars);

    // Same asymmetric-bound convention as resolveRandomStretchPercent -
    // Interval's own neutral point is also 100% (unchanged), not 0 - draws
    // uniformly between 100% and `boundPercent`. Salt 9.
    float resolveRandomIntervalPercent (int instanceSeed, int phraseCounter, float boundPercent);

    // --- Quantized Stretch (see Docs SS16) ----------------------------------
    // The fixed vocabulary of "notation-friendly" Stretch ratios (%): simple
    // integer relationships only - the binary family (powers of 2: 25/50/
    // 100/200/400, clean diminution/augmentation), the ternary/compound
    // family (thirds and the dotted-note ratio: 33.3/66.7/150/300), and two
    // bridge ratios (75/133.3, a 3:4 relationship). An in-between percentage
    // like 137% rescales a phrase's timing off any grid a notation program
    // can render cleanly - this vocabulary exists so Quantized mode can
    // restrict Stretch to ratios that stay notatable, the same "binary vs
    // ternary family" framing OrchQuantizer's own rhythm engine already
    // uses, applied here to a scaling factor instead of a beat subdivision.
    const std::vector<float>& quantizedStretchRatios();

    // Snaps `percent` to the nearest ratio in quantizedStretchRatios() - the
    // resolution Quantized mode applies to a FIXED (non-random) Stretch
    // value: aim the slider anywhere, the value actually used always lands
    // on a notatable ratio.
    float snapToQuantizedStretch (float percent);

    // Resolves the ACTUAL stretch percentage when Random AND Quantized are
    // BOTH on: draws UNIFORMLY from quantizedStretchRatios() restricted to
    // whichever side of 100% `boundPercent` points toward (same bound
    // convention as resolveRandomStretchPercent) - deliberately narrows the
    // draw to the legal ratio set directly, rather than drawing a
    // continuous value and snapping it afterward, which would silently bias
    // the result toward whichever ratio sits nearest the middle of the
    // continuous range instead of giving every legal ratio an equal chance.
    // Salt 7, independent of every other seeded draw in this file.
    float resolveRandomQuantizedStretchPercent (int instanceSeed, int phraseCounter, float boundPercent);
}
