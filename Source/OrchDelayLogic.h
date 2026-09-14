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
// does not loop/repeat a phrase, and it does not talk CC-to-MPL - the three
// v1 transforms (Transpose/Retrograde/Inversion) are reimplemented here
// directly on the buffered note data, not routed through an external engine.
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
        kTransformInversion = 3
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
    // scheduledFirePpq = phraseStartPpq + holdBars * beatsPerBar using the
    // METER ACTIVE AT THIS CALL (capture time), never re-derived later - see
    // Docs SS3 for why fire-time meter changes must not retroactively change
    // what "N bars" meant. Sets `phrase.closed = true`.
    void closePhrase (Phrase& phrase, int holdBars, double beatsPerBar);

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

    // Dispatches to the right transform above by `TransformKind`; kTransformNone
    // returns the input unchanged (a plain copy).
    std::vector<HeldNote> applyTransform (const std::vector<HeldNote>& notes, int transformKind,
                                          double phraseStartPpq, double phraseEndPpq, int transposeSemitones);

    // Resolves `phrase`'s chosen transform AND the schedule-time shift
    // (scheduledFirePpq relative to phraseStartPpq) into a list of
    // INDEPENDENTLY-fireable output notes, computed ONCE - each note's
    // outputOnsetPpq/outputOffPpq is a final, absolute ppq position. Call
    // this once when a phrase is scheduled (right after chosenTransform is
    // resolved); firing later just checks each note's own outputOnsetPpq
    // against the current block, one at a time - never bundles a whole
    // phrase's notes into a single block's emission (see Phrase's own doc
    // comment for why that used to be a real bug).
    std::vector<ScheduledNote> buildOutputNotes (const Phrase& phrase, int transposeSemitones);

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
    // transforms. If applyAny, an equal-weight 3-way pick among the v1
    // transforms (no evidence yet to favor one - see Docs SS7).
    // `instanceSeed`: this OrchDelay instance's own identity (0-127 param).
    // `phraseCounter`: increments once per phrase closure - takes the role
    // OrchGate's broadcast "mode" CC plays, since v1 has no OrchConductor
    // bridge to hash against.
    TransformProposal proposeTransform (int instanceSeed, int phraseCounter, float restlessness);

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
}
