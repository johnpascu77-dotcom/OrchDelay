# OrchDelay — scoping (v1 built)

Status: **v1 built 2026-09-14, build-clean** - VST3 and Standalone both compile with zero errors/
warnings on the first full pass; the pure-logic console check (`OrchDelayLogicCheck`, 32 assertions)
passes cleanly. **Not yet live-tested in a real DAW/rig** - a real Bitwig test (play a phrase,
confirm it echoes back exactly `holdBars` bars later, sample-accurately) is the next required step
before calling v1 actually done, matching this ecosystem's own established discipline (every real
bug in a sibling plugin was found against real use, never trusted from build-success alone).

## SS1. What this is

A "responsorial delay" / live-coherency device for the Orch System's note path: buffers a live-
played phrase, holds it for exactly `holdBars` bars, then fires it back once - a one-shot echo, not
a looping/repeating delay. While held, the phrase may be transformed as a whole (Transpose,
Retrograde, or Inversion in v1) before it fires, chosen by hand or "proposed" via a seeded
`restlessness` knob.

Genesis: the user described this early in the same design conversation that produced OrchQuantizer
("a smarter delay plugin, that would keep the incoming signal in a buffer, then fire it after (and
for) a set number of bars, emulating a responsorial effect... it might also do some MPL-type
operations, by choice or by own proposal"), then explicitly deferred it behind the quantizer
("rhythm quantizer first, as a new sibling tool"). Picked back up once OrchQuantizer's core behavior
was validated against real captures through 10 commits.

## SS2. Scope boundary

**This plugin holds a captured phrase and fires it back once, transformed or not. It does not
loop/repeat a phrase, and it does not talk CC-to-MPL** - the v1 transform vocabulary
(Transpose/Retrograde/Inversion) is reimplemented directly on the buffered note data in
`OrchDelayLogic.cpp`, not routed through an external pattern engine, since OrchDelay processes
arbitrary live-played material with real timestamps, not a fixed step grid.

Four architectural forks were resolved directly with the user via `AskUserQuestion` before any code
was written (see the session's own plan document for the full reasoning): pure MIDI effect (matches
every Orch sibling - the "instrument-with-MIDI-out" precedent in project memory turned out to
describe Composer Mastermind, a different plugin, not this family); name OrchDelay; one-shot echo
(no looping/feedback); v1 transform set limited to Transpose+Retrograde+Inversion (Rotation/Length/
M7 deferred); and live notes are fully swallowed into the buffer (only the delayed echo sounds, no
doubled live+echo texture).

## SS3. Timing - built from scratch, no existing precedent to copy

Confirmed via direct source reading this session: OrchConductor does **not** track bars from the
host transport at all - its "Narrative Position" is a manually/CC102-driven 0..1 knob, not derived
from ppq. The actual reusable transport-polling pattern came from OrchCapture
(`OrchCaptureProcessor.cpp` ~line 219): poll `getPlayHead()->getPosition()` every block for
`isPlaying()`/`getBpm()`/`getPpqPosition()`/`getTimeSignature()` (pull-only, no JUCE change
notification), `ppqPerSample = (bpm/60)/sampleRate`, `beatsPerBar = numerator*4.0/denominator`.

**Fire-time meter is resolved explicitly at capture time, not fire time.** `scheduledFirePpq` is
computed once, at phrase closure, using whatever meter is active right then. If the meter changes
before the scheduled fire arrives, the ppq offset is NOT re-derived - "N bars later" is a promise
made at the moment the phrase was played, and re-deriving it from a later meter would silently
change what "N bars" meant after the fact, with no way for the player to have anticipated it.

No host transport at all (e.g. Standalone with no clock): OrchDelay stays fully inactive and passes
MIDI straight through untouched, rather than guessing bar position from sample counts.

## SS4. Transform vocabulary - two deliberate divergences from MPL, both documented

MPL's own Transpose/Inversion/Retrograde math (`PluginProcessor.cpp` in the sibling MPL project)
was read directly this session as the vocabulary precedent. Transpose ports over unchanged (additive
semitone shift, clamped `[0,127]`). Inversion and Retrograde do NOT port over unchanged - both are
deliberate, reasoned adaptations:

- **Inversion axis = the phrase's own first/anchor note, not MPL's fixed axis=60 (middle C).** MPL's
  fixed axis suits editing a visible on-screen step pattern; OrchDelay processes arbitrary live-
  played material in any register, and a fixed axis=60 would throw a phrase captured entirely below
  C2 into a wildly different, possibly unplayable register. Anchoring to the phrase's own first note
  keeps the echo in roughly the register the player was actually in.
- **Retrograde = whole-phrase time reversal, not MPL's step-grid read-position reversal.** MPL's own
  Retrograde reverses playback READ POSITION within a fixed 16-step grid - meaningless for an
  arbitrary-length, real-timestamp phrase. OrchDelay's version reflects every note's onset around
  the phrase's own midpoint (`newOnset = phraseStart + phraseEnd - (onset + duration)`), preserving
  each note's own duration - the natural generalization of "retrograde" from a grid domain to a
  continuous-timestamp domain, not an imperfect port.

Both divergences are verified by dedicated `OrchDelayLogicCheck` assertions - the Inversion test
specifically includes an assertion that would FAIL under MPL's own fixed-axis-60 convention, making
the deliberate divergence machine-verifiable rather than just asserted in a comment.

## SS5. Restlessness - synthesized from two of three existing idioms found this session

Three distinct "0..1 restlessness knob, deterministic, diverge from a safe/verbatim baseline"
mechanisms already exist in this codebase (OrchConductor's `generateNarrativeLane`: `juce::Random`
seeded fresh per click, restlessness thresholds a hold/reprise-vs-fresh-pick choice; Composer
Mastermind's `ArcShapeLibrary`: `std::mt19937` with an explicit seed, restlessness scales continuous
additive jitter; OrchGate's `resolveResponseOverlay`: a stateless FNV-1a hash, no RNG object at all,
seeded by a broadcast CC + this instance's own CC number). OrchDelay synthesizes two of them:
OrchConductor's **threshold-a-choice** mapping (restlessness IS the probability of proposing a
transform at all - "verbatim vs. diverge" is inherently discrete here, not a continuous curve) +
OrchGate's **stateless hash** (`odly::fnv1aHash`, byte-for-byte ported from
`OrchGateProcessor.cpp`'s own hash lambda - no stored seed, no drift across reloads).

`instanceSeed` (a dedicated 0-127 parameter, "Randomize" button in the editor) plays the role
OrchGate's own `ccNumberParameter` plays as instance identity. `phraseCounter` (increments once per
phrase closure, not persisted) takes the role OrchGate's broadcast "mode" CC plays, since v1 has no
OrchConductor bridge to hash against yet.

## SS6. Stuck-note-cleanup discipline - a mandatory constraint borrowed from OrchPiano's own history

OrchPiano's devlog documents the identical failure class hit three separate times: identifying a
buffered note occurrence by `(channel,pitch)` alone (fixed by giving every `HeldNote` a monotonic
`seq`), and a "damp but don't clear" cleanup path split from a differently-timed "clear" path
(fixed here by writing exactly one `drainAndSilence()` helper that always does both in the same
call - stop, restart, rewind, and bypass all route through it). `OrchDelayLogicCheck` reproduces
OrchPiano's own historical bug shape directly (two overlapping same-pitch notes) to keep this
regression-tested, not just designed-in and hoped for.

**No `setLatencySamples()`.** OrchPiano tried this for a far smaller lookahead and reverted it after
hitting Bitwig's ~2-second latency-compensation ceiling. A multi-bar hold (a 4-bar hold at 120bpm/
4-4 is already 8 seconds) would blow through that ceiling almost immediately. OrchDelay's hold is
undisguised - the user experiences it as "the note plays back later," not as host-compensated
latency - so nothing is reported via `setLatencySamples()`, and no `delayCompensationCc`-style
broadcast either (OrchPiano's version exists because ITS delay is an incidental processing artifact;
OrchDelay's hold is the entire deliberate musical point of the plugin).

## SS7. Explicitly out of scope for v1 (record here, don't silently drop)

- Rotation, Length, M7 transforms - user's own settled v1 cut; `chosenTransform` stays a plain int
  dispatch so adding them later needs no redesign.
- Looping/feedback re-firing - one-shot only, user's own settled v1 cut.
- Any CC bridge to OrchConductor - no relocatable CC params, nothing reserved; add only once there's
  a concrete reason for OrchConductor to steer this.
- Live pass-through alongside the echo - not even a parameter in v1, just the fixed "swallow into
  buffer" behavior (user's own settled choice). A future toggle is a small, easy addition if wanted.
- Waiting for a straddling held note's real note-off before closing its phrase - a fixed 0.5-beat
  fallback instead; "properly wait" risks one stuck key hanging phrase closure indefinitely.
- Per-transform weighting in the 3-way proposal - equal 1/3 weighting, no evidence yet to favor one.
- Pending-phrase-queue visualization in the editor - a minimal editor (parameter controls + a
  one-line transport-status label) shipped for v1.

## SS8. Verification status

`OrchDelayLogicCheck` (32 assertions): bar-length-ppq math across 4 meters, phrase-boundary grouping
(below/at/zero-gap threshold, single-note phrase), the FIFO seq-matching discipline under an
overlapping-same-pitch scenario, the 0.5-beat fallback-duration/closure behavior, scheduledFirePpq's
meter-frozen-at-closure math, Transpose clamping at both ends, Inversion's phrase-own-axis (with the
would-fail-under-MPL's-convention check), Retrograde's exact time-reversal formula,
`proposeTransform` determinism and cross-instance divergence, and restlessness threshold sanity at
0/0.5/1. All passing as of 2026-09-14.

**Not yet done, required before v1 is actually "done"**: confirm `restlessness=0` stays verbatim and
raising it starts producing transforms; confirm transport-stop mid-hold discards cleanly with no
stuck notes; confirm a loop/rewind purges pending phrases without leaving anything stuck.

## SS9. First live test in Bitwig (2026-09-14) - two real bugs found and fixed

The 4-bar hold/fire timing itself worked correctly on the very first live test ("It landed well. 4
bars successfully delayed"). But two real bugs surfaced that no synthetic test had caught, because
both only manifest against genuinely continuous transport time, which `OrchDelayLogicCheck`'s
hand-constructed ppq values never exercised realistically:

**Bug 1 - phrase-gap detection used onset-to-onset spacing, not rest.** `captureEvent`'s gap check
compared a new note's onset directly against the PREVIOUS note's onset, not its end. For 4 live
quarter notes played back-to-back (onset spacing ~1 beat, matching the default 1.0-beat Phrase Gap),
this meant nearly every note-to-note transition looked like a phrase boundary even though the actual
rest (silence) between notes was near zero - fracturing what should have been one 4-note phrase into
up to 4 separate one-note phrases. Live symptom: "for 4 quantized quarters I get only 3 delayed" -
the reported 3rd fired echo was actually 3 separate single-note phrases firing at 3 different times;
the 4th note's own phrase never closed (see Bug 2) and got silently wiped on stop, never echoing at
all - hence "only 3."

**Bug 2 - a phrase could only close via a SUBSEQUENT note-on's gap check, never on its own.**
`captureEvent` is the only place phrase closure was ever evaluated, and it only runs when a new MIDI
event arrives. A phrase whose last note is never followed by anything else - a finite clip that ends,
or the last take before the player simply stops - had no way to close: there was no next note-on to
notice the gap had elapsed. It just sat open forever, then got silently discarded (per the deliberate
"discard, don't force-fire" stop behavior) the moment the transport stopped. This is the real
explanation for "responds only to live input; not receiving from other tracks or from a recorded midi
on its own track" - the plugin WAS receiving and correctly swallowing that MIDI (a MIDI-effect insert
can't structurally distinguish live-played notes from clip-played ones; both arrive identically in
`processBlock`'s `MidiBuffer`), it just never fired anything back, because a finite clip's few notes
followed by silence never produced a follow-up note-on to trigger closure. Live playing happened to
mask this because the player naturally keeps feeding new notes, which closed out trailing phrases via
the normal captureEvent gap check - purely incidental, not by design.

**Fix**: (1) `captureEvent`'s gap check now measures rest from the previous note's actual END
(`onset + duration`, or the current event's own ppq - i.e. zero rest - if that note has no note-off
yet, since it's still sounding) rather than onset-to-onset. (2) A new `odly::checkPhraseTimeout()`
function, called once per block in `processBlock` after the normal event-capture loop (whenever
`playing` is true), proactively closes `openPhrase` once the rest since its last note's end reaches
`phraseGapBeats` - with no new note-on required. This uses the exact same `closePhrase()` as the
note-on-triggered path, so `scheduledFirePpq` math is identical either way; only the trigger differs.

Both fixes are covered by dedicated `OrchDelayLogicCheck` regression tests (36 assertions total,
4 new): a direct reproduction of "4 near-legato quarter notes at 1-beat spacing stay ONE phrase, not
4," and 3 covering `checkPhraseTimeout` (closes on its own past the threshold with no follow-up note;
does NOT close early; does not fire on a still-sounding note or an empty phrase). All passing.

## SS10. Second live test (2026-09-14) - stop discarded the last phrase before it could close

Rebuilt with the SS9 fix, reinstalled - and got "no answer whatsoever" this time, worse than before.
Root cause: `checkPhraseTimeout` only runs while `playing` is true (per its own contract - it can't
evaluate elapsed transport time while nothing is advancing). But `stoppedPlaying` unconditionally
DISCARDED `openPhrase`. Stopping the transport shortly after finishing a phrase - the single most
natural way a player signals "that take is done" - meant the last phrase never got the ~1 real beat
of continued playback it needed to close via `checkPhraseTimeout`, and never received a follow-up
note either. It just vanished, every time, on every stop. The SS9 fix made phrase closure correctly
require real elapsed silence instead of a lucky onset-spacing coincidence - but stopping shortly
after playing was never given a chance to provide that silence.

**Fix**: `stoppedPlaying` now closes and SCHEDULES the still-open phrase (via `closePhrase`, same as
any other closure path) instead of discarding it - it does not fire immediately; the normal
`scheduledFirePpq`-vs-block-range check in the fire loop still governs exactly when it sounds, same
as always. A phrase that was ALREADY closed and mid-hold *before* this stop is still discarded, not
force-fired - that original SS2 reasoning is unchanged and still correct; it only ever applied to
material already scheduled, and the newly-closed-at-stop phrase is scheduled *after* that clear runs,
so it survives.

This only works, though, if an ordinary "stop, then resume from the same position" isn't
misidentified as a rewind and purged a moment later. It would have been: `blockEndPpq` was being
extrapolated forward by `numSamples*ppqPerSample` every block regardless of `playing`, so during any
stopped block it kept drifting further ahead of the actual (frozen) transport position - by the time
playback resumed, `lastBlockEndPpq` would sit well ahead of the freshly-read (correct, unchanged)
resume position, which is exactly what the rewind check looks for. **Fixed alongside**: `blockEndPpq`
now equals `blockStartPpq` (no extrapolation) whenever `playing` is false, so `lastBlockEndPpq` stays
pinned at the true frozen position throughout a stop, and an ordinary resume no longer looks like a
backward jump. With that fixed, `rewound`'s own check no longer needs to be gated to `playing &&
wasPlaying` (i.e., "only mid-playback") - it now also correctly catches a playhead relocated backward
*while stopped*, and the redundant separate `startedPlaying`-triggered full-clear (originally "fresh
take starts clean") was removed - a genuine relocate-and-restart is already caught by the generalized
`rewound` check, and an ordinary resume from exactly where playback paused no longer needs, or gets,
a clear at all.

This is a processor-level integration behavior (state across the stop/resume transport transition,
`wasPlaying`/`lastBlockEndPpq` bookkeeping), not something expressible as a pure `odly::` function, so
it isn't covered by `OrchDelayLogicCheck` - noted here as a real verification gap rather than forcing
an awkward test.

## SS11. Third live test (2026-09-14) - diagnostic counters + the actual root cause found

Added 3 session-lifetime counters to the editor status line (`notesCapturedForUi`/
`phrasesClosedForUi`/`phrasesFiredForUi`, reset only on `prepareToPlay`) specifically to stop guessing
blind after two silent-result rounds. Third test result: **captured 4, closed 1, fired 0, pending 0**.
This was immediately diagnostic: all 4 notes correctly grouped into ONE phrase (confirms the SS9 gap
fix works), that phrase correctly closed and got scheduled (confirms the SS10 stop-schedules-instead-
of-discards fix works) - but it vanished before firing, with nothing left pending. Something purged it
between scheduling and firing.

**Root cause**: Bitwig's own Stop button returns the playhead to the play-start position by default -
unlike most DAWs, which just pause in place at the stop point. The `rewound` check (SS10's own
generalization, deliberately no longer gated on `playing && wasPlaying` so it could also catch a
backward relocation made while stopped) was still unconditional in when it ACTED - so the very same
Stop press that correctly closed-and-scheduled the open phrase (via `stoppedPlaying`, SS10) was
immediately followed by Bitwig's playhead snapping back to the start, which read as a backward jump
and purged `pendingPhrases` right back out again, all within the handling of one single Stop press.
The phrase never had a chance to actually sit in the pending queue where the UI (or anything else)
could observe it.

**Fix**: `rewound`'s purge is now gated on `playing` (`if (rewound && playing)`) - a backward jump is
only acted on while actually playing THROUGH it. A relocation that happens purely while stopped needs
no purge: nothing depends on ppq continuity until playback resumes, and `blockEndPpq`'s own
not-playing branch (SS10) already keeps `lastBlockEndPpq` pinned at the true frozen position
throughout a stop, so a genuine resume-from-an-earlier-point is still caught correctly the moment
`playing` goes true again - this fix only changes behavior for a jump detected while stopped, which
previously purged for no operational reason (nothing was going to fire while stopped anyway).

**Fourth live test, same session**: the `playing`-gated rewind fix did NOT resolve it - identical
signature recurred (`captured 4, closed 1, fired 0, pending 0`) from a single clean stop with no other
transport action, confirmed by the user directly ("no stop was involved [before]. Only then I hit
stop"). Re-examining `stoppedPlaying` found the real bug was simpler and in a different line entirely:
it unconditionally cleared `pendingPhrases` at its own top (the original v1 "discard anything mid-hold
at stop" policy), UNCHANGED by the SS10/SS11 fixes. If the host ever reports `isPlaying()` flipping
more than once for what is really one stop gesture - plausible, unconfirmed, now instrumented via a
new `totalStopEventsUi` counter - each extra `stoppedPlaying` transition would silently wipe out the
very phrase the first one had just closed and scheduled, before anything could observe or fire it.

**Fix**: removed the clear entirely. `stoppedPlaying` now only ever closes the still-open phrase (a
safe no-op once it's already empty, including on a repeated/glitchy transition) - it no longer touches
`pendingPhrases` at all. The only remaining purge path is the genuine `rewound && playing` case. This
retroactively obsoletes the original SS2 "discard mid-hold at stop" policy entirely, not just for the
still-open phrase (SS10) - there was never a real need to discard ALREADY-scheduled material either,
once firing was already deferred to the normal schedule rather than forced immediately.

Added 3 more counters (`stopEventsForUi`/`rewindDetectedForUi`/`rewindActedForUi`) so the next test, if
this still doesn't resolve it, will be immediately conclusive rather than another guess. Rebuilt +
reinstalled, Build ~17:23 UTC.

**Fifth live test**: a fully clean 8-bar pass (confirmed no loop region, no stop, no rewind at all -
`rwSeen 0`) STILL produced silence: `cls 1, fire 0, pend 1` the entire time, despite playback clearly
sweeping well past where the phrase should have fired. Widened the fire loop's condition from an exact
`[blockStartPpq, blockEndPpq)` window match to simply `scheduledFirePpq < blockEndPpq` (fires in the
first block that notices something is due, immune to any gap between two blocks' own windows), and
added `lastScheduledFirePpqUi`/`furthestBlockPpqUi` ("sched X reached Y") to see the exact numbers
directly instead of guessing further. Also widened the note-off emission loop's window check the same
way, closing the same class of gap risk there (a latent stuck-note risk, never actually observed but
real).

**Sixth live test, this fix**: phrases finally fired - "the good news is that they fire after the
specified hold bars correctly" (Hold Bars=2, scheduled at ppq 8.0, fired once playback reached it).
But: "all notes fire as cluster" - all 4 notes of the echoed phrase sounded simultaneously instead of
preserving the original rhythm.

## SS12. The actual cluster bug - firing bundled a whole phrase into one block

Root cause: the fire loop treated "is this phrase due" as one atomic, whole-phrase check, then emitted
EVERY note of the phrase within that SAME triggering block - each note's own onset got clamped into
that one block's narrow sample range via `onSample`'s `jlimit`, collapsing 4 notes spread across a full
bar into a single instant. This was invisible in every earlier test: the SS9-era broken gap-detection
fractured every phrase into single notes, so there was never more than one note in a phrase to
collapse - SS9's own fix (correctly grouping a real phrase back together) is what finally exposed this
pre-existing, previously-unreachable bug.

**Fix**: added `odly::ScheduledNote` + `odly::buildOutputNotes()` - the transform and the schedule-time
shift are now resolved ONCE, at scheduling time (inside `resolveAndScheduleTransform`, which now also
takes `transposeSemitones`), into a list of INDEPENDENTLY-fireable notes stored on `Phrase::outputNotes`
- each carrying its own absolute output onset/off ppq and an `emitted` flag. The fire loop now checks
and emits each note on its own, across however many blocks are needed for playback to reach each one,
rather than bundling a whole phrase's notes into whichever single block first notices the phrase
overall is due. `totalPhrasesFiredUi` now increments once the LAST note of a phrase is emitted, not the
first.

A dedicated regression test (`buildOutputNotes`) reproduces the exact bug directly: 4 notes a beat
apart must each get their own independent output onset (verified exactly, not just "looks staggered"),
not all collapsed onto `scheduledFirePpq`. 42 assertions total, all passing. Rebuilt + reinstalled,
Build ~17:50 UTC.

## SS13. Hold Bars=1 edge case - scheduledFirePpq re-anchored to phraseEnd + floored at closure

Seventh live test confirmed SS12's fix: Hold Bars 2 and 3 both echoed correctly, preserving the
original rhythm. But Hold Bars=1 still misbehaved: the first (and sometimes second) note fired almost
simultaneously instead of spread across the bar - worse with a larger Phrase Gap, better (but not
fully clean, and with a shortened first note) with a smaller one.

Root cause: `scheduledFirePpq` was computed as `phraseStart + holdBars*bar`. But a phrase can't
actually CLOSE until `phraseGapBeats` of rest has elapsed PAST ITS OWN END - so whenever `holdBars`
(in bars) is comparable to or shorter than `phraseGapBeats` (in beats), the nominal echo start point
can fall BEFORE the phrase is even known to be closed. By the time closure finally happens, one or
more notes' own intended output onsets are already in the past, so SS12's per-note fire loop (correctly
now) fires them all in the very next block it gets - which just happens to be "immediately," collapsing
them together despite per-note firing being otherwise correct.

**Fix**: `scheduledFirePpq` is now anchored to `phraseEndPpq` instead of `phraseStartPpq` (also the more
natural "responsorial" reading - answer N bars after the phrase is DONE, not N bars after it began),
and floored at `nowPpq` (the closure call's own ppq) so an echo can never be scheduled before its
source material even exists, for any holdBars/phraseGapBeats combination. `closePhrase()` gained a
`nowPpq` parameter; all 3 call sites (`captureEvent`'s own gap-triggered close, `checkPhraseTimeout`,
and the processor's stop-triggered close) now pass the real ppq at closure. 2 tests updated/added
directly covering the new anchor and the floor. 44 assertions, all passing. Rebuilt + reinstalled,
Build ~18:03 UTC.

**RESOLVED**: live-retest confirmed Hold Bars=1 now works correctly ("Working well" across many
takes, status line showed every closed phrase eventually firing). The whole SS9 through SS13 arc
(gap detection, stop scheduling, rewind gating, stop's own clear, the fire condition, per-note
firing, and the schedule-time anchor/floor) is done. 11 commits (`09218d4`..`e3d114d`) pushed to
`github.com/johnpascu77-dotcom/OrchDelay` per explicit user request.

Also explained during this test: a very small Phrase Gap (0.25 beats) can fragment one intended
phrase into several if the player's own notes have tiny natural gaps inside them - each fragment
gets its own independently-scheduled echo, landing close together but misaligned ("enters early,
with overlapping notes"). This is a tuning consideration, not a bug - documented here so a future
session doesn't mistake it for a regression.

## SS14. v1.1 - transform vocabulary expansion (Rotation, Length, M7) + Random Transpose

User asked to bring the previously-deferred transforms (Docs SS7: "Rotation, Length, M7 - user's own
settled v1 cut") into scope, plus a way to randomize the Transpose amount per phrase rather than
always using one fixed value. Three design forks resolved directly with the user via
`AskUserQuestion` before writing code: keep the Transpose slider's existing continuous range and add
a separate "Random" toggle (not a discrete named-interval dropdown); Random re-rolls a fresh value
every echoed phrase via the same deterministic-hash mechanism as Restlessness's own transform pick,
not only on a manual button press; and yes, bring Rotation/Length/M7 into scope now rather than
deferring them further.

**Rotation and Length needed real domain adaptation** (re-read MPL's own source,
`PluginProcessor.cpp`, to ground both rather than guessing):

- **Rotation**: MPL's own formula is `sourceStepIndex = playbackStepIndex - rotation`, wrapped mod
  the pattern's loop length - a cyclic reassignment of which STORED step's content sounds at a given
  playback position. OrchDelay's `applyRotation` is a direct, faithful port to a variable-length note
  list instead of a fixed 16-step grid: slot `i` (in onset order) keeps its OWN onset/duration (the
  phrase's rhythmic skeleton is never touched), but takes its pitch/velocity/channel from slot
  `(i - steps) mod noteCount`. Wraps automatically to whatever length the phrase actually has,
  including a graceful no-op on a 0/1-note phrase, matching MPL's own no-op-on-a-1-step-loop
  behavior.
- **Length**: MPL's own Length clamps how many of a pattern's steps play before it loops back -
  meaningless verbatim for a one-shot device with no loop to clamp. Generalized as a direct
  truncation instead: `Length%` keeps the first `round(noteCount * percent/100)` notes (at least 1,
  by onset order), dropping the rest from the echo entirely - the natural reading of "how much of the
  captured material actually plays" once looping itself is off the table.
- **M7**: `pitchClass' = (pitchClass * 7) mod 12`, octave register left alone - ported byte-for-byte
  from MPL's own implementation with NO domain adaptation needed, since it's a pure per-note pitch
  operation with no timing/grid dependency at all (the one transform, of the three added here, that
  is a direct unmodified port rather than a reasoned generalization).

`TransformKind` extended to 7 entries (`kTransformRotation=4, kTransformLength=5, kTransformM7=6`).
`proposeTransform`'s equal-weight random pick widened from 3-way to 6-way (Transpose/Retrograde/
Inversion/Rotation/Length/M7 - kTransformNone is never itself proposed, matching the original
design). The `Transform` choice parameter grew from 5 to 8 items (`Follow Restlessness/None/
Transpose/Retrograde/Inversion/Rotation/Length/M7`).

**Random Transpose**: a new `transposeRandom` bool parameter. When on, the amount actually applied
per phrase is `odly::resolveRandomTransposeSemitones(instanceSeed, phraseCounter, transposeSemitones)`
- the SAME deterministic FNV-1a hash construction as `proposeTransform` (salt 3, kept independent of
the transform-choice draws at salts 1/2), drawn uniformly from `[-|transposeSemitones|,
+|transposeSemitones|]`. The existing "Transpose (semitones)" slider itself becomes the symmetric
RANGE bound in this mode rather than a literal fixed amount - raising/lowering it still controls "how
far," just as a ceiling instead of an exact value. Reload-stable and deterministic like every other
seeded choice in this plugin: same seed + same phrase count always redraws the same amount.

`resolveAndScheduleTransform` now also builds `phrase.outputNotes` using the resolved (possibly
random) transpose amount plus the raw `rotationSteps`/`lengthPercent` parameter values, read directly
from their own parameters (only `transposeSemitones` is still passed in, since the stop call site
needs its own local re-read matching `holdBarsAtStop`'s existing pattern).

New parameters: `rotationSteps` (Int, -16..16, default 1), `lengthPercent` (Float 1-100%, default
50%), `transposeRandom` (Bool, default false). Editor grew a "Random" toggle next to the Transpose
slider, plus Rotation and Length slider rows; window height 640->720 to fit.

21 new/updated test assertions (65 total, all passing): `applyRotation` (shift-by-1 exact mapping,
onsets unchanged, 0-steps no-op, full-wrap no-op, single-note no-op), `applyLength` (100%/50%/1%/
empty), `applyM7` (fixed point at pitch class 0, octave-preserving mapping, self-inverse round-trip),
`applyTransform` dispatch for all 3 new kinds, `proposeTransform`'s 6-way coverage (every kind gets
picked across a large sample), and `resolveRandomTransposeSemitones` (determinism, range bound, zero-
range collapses to 0, both signs appear across a sample). Rebuilt + reinstalled, Build ~19:43 UTC.
**Not yet live-tested** - required before calling v1.1 actually done, per this repo's own established
discipline.
