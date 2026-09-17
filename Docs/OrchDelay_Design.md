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
range collapses to 0, both signs appear across a sample).

**RESOLVED**: live-tested successfully, including with polyphonic material (not just single-line
phrases) - unplanned but welcome validation that the note-list-based transform math generalizes fine
beyond the monophonic case it was designed against. User asked one follow-up about Length's
percentage semantics (worried intermediate values might produce awkward-to-quantize rhythms) -
clarified that Length only truncates NOTE COUNT, never rescales timing, so surviving notes keep their
exact original onset/duration; no quantization risk exists under this design. That question led
directly to SS15 below.

## SS15. Random toggles for Rotation/Length + a genuine time-stretch transform (Stretch)

Two follow-up requests in the same live-test conversation:

**Random Rotation / Random Length** (commit `f005b14`): same convention as Random Transpose - the
slider becomes a range bound instead of a literal value. Rotation is symmetric like Transpose
(`odly::resolveRandomRotationSteps`, salt 4, `[-|Rotation|, +|Rotation|]`); Length has no negative/
symmetric meaning (always 0-100%), so its slider becomes a CEILING instead
(`odly::resolveRandomLengthPercent`, salt 5, `[1%, Length%]`). 8 new tests (73 total).

**Stretch** (commit pending, this entry): while discussing Length, the user described wanting a
genuine proportional TIME-STRETCH of the echoed phrase - similar to Bitwig's own static MIDI-clip
stretch, but live and per-echo, "not possible to be achieved with Bitwig only" since a clip edit can't
re-decide itself on every single echo (especially combined with Random mode). No precedent to port
from MPL - MPL's own patterns are locked to the host's fixed BPM/step grid with nothing to stretch;
this transform only exists because OrchDelay owns real, freely-rescalable timestamped material.

`odly::applyStretch(notes, phraseStartPpq, stretchPercent)`: rescales every note's onset (relative to
the phrase's OWN start - the anchor note never moves) and duration by `stretchPercent/100`. 100% =
unchanged, <100% = faster/shorter, >100% = slower/longer, with every note's RELATIVE rhythmic
position preserved exactly (a straight proportional rescale). Composes for free with the existing
`buildOutputNotes` schedule-time math, which only ever reads each note's offset from `phraseStartPpq`
- a stretched offset lands in the right place with zero special-casing. `TransformKind` extended to 8
entries (`kTransformStretch=7`); `proposeTransform`'s equal-weight pick widened 6-way -> 7-way;
`Transform` choice parameter grew from 8 to 9 items.

**Random Stretch** needed its own bound convention, distinct from both existing patterns: Stretch's
neutral point is 100% (not 0 like Transpose/Rotation, not a 0-100% fraction like Length), so
`odly::resolveRandomStretchPercent` (salt 6) draws uniformly between 100% and the slider's own bound -
whichever side of 100 the bound sits on sets the direction (a 200% bound wanders slower/longer only; a
50% bound wanders faster/shorter only; exactly 100% always resolves to 100%, no range).

New parameters: `stretchPercent` (Float, 25-400%, default 100%), `stretchRandom` (Bool). Editor gained
a Stretch slider + Random toggle row; window height 720->780 to fit. 14 new test assertions (87 total,
all passing): `applyStretch` (100%/50%/200% exact math, anchor-note-never-moves, relative-to-
phraseStart not absolute-zero, out-of-range clamping), dispatch, 7-way proposal coverage, and
`resolveRandomStretchPercent`'s asymmetric-bound behavior in both directions.

**RESOLVED**: live-tested successfully - "Stretch works as expected." One real finding: in-between
Stretch percentages (anything besides roughly 50/200/400%) produce musically interesting results
that land "way off any binary or ternary grid," making them unusable once the echoed material needs
to go through Dorico. User's own proposed fix, confirmed and scoped: a Free/Quantized toggle. Before
building it, the user also shared a large (21K-line) archived ChatGPT transcript from an earlier,
pre-Claude design pass on this same "smart delay" concept (working title "Parrot") for review - see
`project_orchdelay_concept.md` in Claude's own memory for the full mined-idea report (several genuine
feature ideas surfaced, deferred to a later round; the transcript also independently converged on the
same "restrict to a legal ratio vocabulary, don't just snap a continuous value" design used below,
which is why SS16 cites it as validation, not just a coincidence).

## SS16. Quantized Stretch - restricting Stretch to a notation-friendly ratio vocabulary

Root problem: `Stretch` rescales a phrase's timing by an arbitrary percentage (25-400%, continuous).
Applied to already-clean, quantized source material, a percentage like 137% produces output timing
that's mathematically exact but expressible on no ordinary notation grid - not a bug, but a real
practical limit once the echoed phrase needs to reach Dorico through this ecosystem's downstream
notation pipeline (the same concern [[project_orchquantizer_concept]] exists to solve for raw capture
timing in general).

**Fix**: a fixed vocabulary of "notation-friendly" ratios
(`odly::quantizedStretchRatios()`) - simple integer relationships only, mirroring OrchQuantizer's own
binary-family/ternary-family framing (N∈{1,2,3,4,6} subdivisions) but applied to a scaling factor
instead of a beat subdivision:
- Binary family (powers of 2, clean diminution/augmentation): 25%, 50%, 100%, 200%, 400%
- Ternary/compound family (thirds, the dotted-note ratio): 33.3%(1:3), 66.7%(2:3), 150%(3:2), 300%(3:1)
- Two bridge ratios (3:4 relationships): 75%, 133.3%

A new `stretchQuantized` bool parameter, editor toggle "Quantize" next to the existing "Random" toggle
in the Stretch row. Four behaviors, composed cleanly with the existing Random Stretch mechanism (same
`resolveAndScheduleTransform` resolution chain, no special-casing elsewhere - Length/Rotation/
Transpose's own Random modes are untouched):
- Neither on: raw slider value, unchanged (today's existing behavior).
- Quantized only: `odly::snapToQuantizedStretch()` snaps the fixed slider value to the nearest legal
  ratio - aim anywhere, the value actually used always lands on a notatable ratio.
- Random only: unchanged from SS15 (continuous draw within the slider's bound).
- **Random AND Quantized together**: `odly::resolveRandomQuantizedStretchPercent()` draws UNIFORMLY
  from the legal ratio vocabulary itself, restricted to whichever side of 100% the slider's bound
  points toward - deliberately NOT a continuous draw followed by a snap, which would silently bias
  the result toward whichever ratio sits nearest the middle of the range rather than giving every
  legal ratio an equal chance. This was the one real design fork in the whole feature, and the
  ChatGPT-transcript review (see above) independently converged on the same answer: when combining a
  randomizing mechanism with a notation-legality constraint, narrow the CHOICE SET directly rather
  than constraining the OUTPUT of an unconstrained draw.

Under Follow Restlessness, both flags apply exactly the same way whether Stretch was reached by
manual choice or by the restlessness-driven random transform pick - no separate interaction logic
needed, since `resolvedStretchPercent` is computed unconditionally every phrase and simply goes
unused if the phrase's resolved transform isn't Stretch.

10 new test assertions (97 total, all passing): the ratio vocabulary's own sanity (includes 100%, real
size), `snapToQuantizedStretch` exact-value/near-value/tie-breaking behavior, and
`resolveRandomQuantizedStretchPercent`'s determinism, exact-legal-value guarantee, bound restriction
in both directions, the 100%-bound no-range case, and multi-value spread across a sample.

**RESOLVED**: live-tested successfully.

## SS17. Overlap Mode + Random/Pause Hold Bars

Prompted by a direct question from the user: what happens when a wide-stretched answer is still
playing and a NEW answer becomes due? Answer, confirmed against the actual code: nothing coordinates
them - `pendingPhrases` and the fire loop check every pending phrase's notes against the current
block every time, with no gate at all, so a new echo starts firing right on top of an old one if
their timing coincides. Deliberately kept as-is for piano-friendly polyphonic material ("the
overlapping ... can be useful as is now"), but the user also wants explicit alternatives, since most
orchestral instruments aren't polyphonic. Two further, smaller requests arrived in the same
conversation: a Random toggle for Hold Bars (matching the pattern already established for Transpose/
Rotation/Length/Stretch), and a dedicated "pause capturing" state via `Hold Bars = 0` so a very
responsive setting doesn't make "the parrot ... too annoying" - a way to give it a rest without a
hard Bypass reset.

**Overlap Mode** - new `overlapMode` choice parameter, 3 values:
- **Overlap** (default) - today's original behavior, unchanged, preserved deliberately for material
  where layering/canon-style overlap is wanted (piano, pad-like material).
- **Wait** - a due answer whose own first note hasn't fired yet is held back while
  `!activeFiredNotes.empty()` (something from an earlier answer is still audibly sounding). The
  moment it's released, `odly::shiftOutputNotes()` shifts its ENTIRE remaining schedule forward by
  however long it waited, preserving its own internal rhythm exactly rather than firing every
  already-overdue note in one clump the instant the coast clears. Confirmed with the user: when
  several answers pile up waiting during one busy period, only the OLDEST releases when it's clear -
  it becomes the new "busy" source for the others, so exactly one voice sounds at a time. This falls
  out for free from checking `activeFiredNotes.empty()` freshly at each phrase's own decision point
  inside the fire loop (never precomputed once per block) - the moment the oldest phrase's first note
  fires, it's already reflected in `activeFiredNotes` by the time the next pending phrase is checked
  in the same pass.
- **Skip** - a due answer that's busy at the exact moment it becomes due is discarded outright,
  permanently (never re-checked once marked `fired=true` with nothing emitted) - it does not wait for
  the busy period to end and then fire late; that would be indistinguishable from Wait mode.

Both Wait and Skip only ever gate a phrase's own FIRST note. Once a phrase has started (any note
already emitted), none of its own later notes are ever re-gated against busy state - only
INTER-phrase collisions are managed; a single answer's own internal texture (e.g. a Stretched
phrase's own notes overlapping each other) is left alone entirely, matching the user's own framing of
the question (one answer colliding with another, not a phrase colliding with itself).

**Random Hold Bars** - new `holdBarsRandom` bool, same convention as Length: drawn per phrase from
`[1, Hold Bars]` via `odly::resolveRandomHoldBars` (salt 8) - deliberately never 0, so a random draw
can never silently re-enable capturing by chance while deliberately paused.

**Hold Bars = 0 - dedicated pause state**: `holdBars` parameter range widened from `1-16` to `0-16`.
When the base (pre-random) value is exactly 0, ALL capture-related logic is skipped entirely for that
block - the note-capture loop, `checkPhraseTimeout`, and the stop-triggered "close the still-open
phrase" logic all sit behind `if (baseHoldBars > 0)` gates. Per the user's own confirmed choice: live
notes continue to be silently absorbed exactly as they always are (never passed through, unchanged
from Docs SS3.2) - Hold Bars=0 is specifically "stop capturing new material," not "let me hear myself
while paused." Already-pending/mid-hold phrases from before the pause are completely unaffected,
since only the fire and note-off loops (unconditional, never gated) touch them - pausing never cancels
an already-promised echo, consistent with every other "don't discard scheduled material casually"
decision already made in this plugin (SS10 especially). A still-open (uncaptured-yet) phrase frozen at
the moment of pausing is left exactly as it was rather than force-closed by a stop event that happens
while paused - it resumes normally the moment Hold Bars is raised back to 1+.

A new `totalPhrasesSkippedBusyUi` diagnostic counter (`skip N` in the status line) tracks Skip-mode
discards, extending the same diagnostic discipline established across SS9-SS12.

12 new test assertions (109 total, all passing): `shiftOutputNotes` (shifts both onset and off,
preserves inter-note spacing) and `resolveRandomHoldBars` (determinism, range bound, ceiling-of-1
edge case, NEVER draws 0, spreads across the full range). The Overlap Mode busy-gating/release/shift
logic itself lives in the processor (real-time `activeFiredNotes` state, not expressible as a pure
`odly::` function beyond the shift itself) and isn't covered by `OrchDelayLogicCheck` - a known
verification gap, same category as the stop/rewind transport-transition logic noted after SS10.
Rebuilt + reinstalled, Build ~16:40 UTC 2026-09-15.

**RESOLVED**: live-tested successfully ("all tested OK"), pushed (`5cff25f..df99c8f`).

## SS18. The "second list" - 5 deferred ideas from the transcript review, taken on together

Before this session, a background agent mined the archived ChatGPT "Parrot" transcript (see SS16's
own lead-in) for ideas not yet built, deferred as a "second list." The user asked how each would
actually behave musically before committing to any of them; given a plain description of each in
action (no implementation detail), the response was to greenlight all 5 and delegate implementation
order. Order chosen deliberately: smallest/most self-contained first, most architecturally
significant (reopens the swallow-only design) last, so each lands on solid ground before the next:
1. Interval expansion/contraction (this entry)
2. Content-aware transform weighting
3. Phrase-quality gate
4. Multi-motive memory bank
5. Overlay/Ducking capture modes

### Interval expansion/contraction (kTransformInterval, 9th transform)

`odly::applyIntervalScale(notes, scalePercent)` scales every note's own interval from the phrase's
OWN anchor note (its first/anchor note, same convention as Inversion) by `scalePercent/100`: 100% =
unchanged, >100% widens the melodic shape's leaps (same contour, reaching further), <100% narrows it
(converging toward the anchor as percent→0, where every note collapses onto the anchor pitch). No
MPL/transcript precedent - the user's own idea, surfaced directly in conversation, not mined from the
transcript. Distinct from every existing transform: Transpose shifts the whole phrase together
(shape unchanged), Inversion mirrors the shape (exact interval sizes preserved) - this is the one
that changes the SIZE of the melodic shape while keeping its up/down silhouette recognizable.

`TransformKind` extended to 9 entries (`kTransformInterval=8`); `proposeTransform`'s equal-weight pick
widened 7-way → 8-way; `Transform` choice parameter grew from 9 to 10 items. New parameters:
`intervalScalePercent` (Float 0-300%, default 150% - deliberately NOT the 100% no-op, so picking
"Interval" from the menu is immediately audible, matching Transpose/Rotation's own choice to default
away from silence) and `intervalRandom` (Bool) - `odly::resolveRandomIntervalPercent` (salt 9) uses
the same asymmetric-bound convention as Random Stretch, since Interval's neutral point is also 100%,
not 0. Editor gained an Interval Scale slider + Random toggle row; window height 860→914.

11 new test assertions (120 total, all passing): `applyIntervalScale` (100%/200%/0%/empty exact math,
anchor-never-moves), dispatch, 8-way proposal coverage, and `resolveRandomIntervalPercent`'s
asymmetric-bound behavior in both directions.

**RESOLVED**: live-tested successfully. Moving on to item 2.

### Content-aware transform weighting

Under Follow Restlessness, WHICH transform gets picked was always a flat equal-weight 1-in-8 chance -
this was the long-open "per-transform weighting" gap noted since v1 (Docs SS7: "no evidence yet to
favor one"). This item directly fills that gap with a concrete, defensible bias instead of arbitrary
per-transform weights: `odly::computePhraseFeatures()` measures a captured phrase's own basic shape
(note count, pitch spread in semitones, density = notes per beat over its own onset span - all cheap
to compute from the already-captured `HeldNote` list, no new state needed), and
`odly::proposeWeightedTransform()` uses those features to bias the pick:
- **Inversion and Interval** favor a WIDE pitch spread - more dramatic to mirror or scale a melodic
  shape that already spans a lot.
- **Rotation** favors a HIGHER note count - needs several notes for a cyclic reassignment to be
  interesting at all.
- **Length** favors HIGH density (busy, many notes in a short span) - more material to meaningfully
  trim.
- **Stretch** favors LOW density (sparse, spread-out material) - more room and time to stretch into.
- **Transpose, Retrograde, and M7** stay at a flat baseline weight - each reads as "always reasonable"
  regardless of a phrase's own content, no clear bias case for either direction.

Implemented as a weighted-random pick (cumulative-weight selection against the same deterministic
hash construction as every other seeded choice here, salt 10) rather than hard categorical rules -
each transform's weight is `1.0 + bonus` where `bonus` is a phrase-feature-derived value in `[0,1]`,
so nothing is ever fully excluded, only made more or less likely. The original equal-weight
`proposeTransform()` is left completely untouched (still independently tested) - a new
`contentAwareWeighting` bool parameter (default ON, a strict improvement over flat-random) selects
between the two at the one call site in `resolveAndScheduleTransform`; turning it off restores the
original purely-uniform pick for anyone who wants that back. Has no effect when Transform is set to
an explicit manual choice (only Follow Restlessness ever reaches this code path at all).

10 new test assertions (130 total, all passing): `computePhraseFeatures`'s own arithmetic (count,
spread, density, empty/single-note edge cases with no NaN/divide-by-zero), and - the assertions that
actually matter here - three STATISTICAL bias-direction checks across large samples, confirming a
dense phrase picks Length more often than an otherwise-identical sparse one, a sparse phrase picks
Stretch more often than an otherwise-identical dense one, and a wide-range phrase picks Inversion more
often than an otherwise-identical narrow one - i.e. the bias actually points the intended direction,
not just "the code runs." Rebuilt + reinstalled, Build ~17:26 UTC 2026-09-15.

**RESOLVED**: live-tested successfully. Moving on to item 3.

### Phrase-quality gate

`odly::computePhraseInterest(notes)` scores a captured phrase 0..1 on purely structural grounds -
never a judgment about musical TASTE, just shape: equal-weighted average of note count (ramps to 1.0
by ~6 notes - doesn't take much to feel "worth it"), pitch variety (distinct pitches / total notes -
several repeats of one pitch scores near 0 regardless of note count), and pitch spread (saturates at
an octave). A new `minimumInterest` parameter (0-100%, default 0% = gate fully disabled - unlike
Content-Aware Weighting, this can actively DISCARD material, so it stays opt-in rather than
defaulting on) is checked once per closure, in a new shared `scheduleClosedPhrase()` helper that now
sits between all 3 closure sites (capture-triggered, `checkPhraseTimeout`, stop-triggered) and the
actual scheduling - a phrase below threshold is still captured and closed (counted in the existing
`totalPhrasesClosedUi`) but never gets an echo scheduled for it at all. This DRYs up code that had
been triplicated at all 3 sites since SS9.

New `totalPhrasesSkippedQualityUi` (`skipQ N`) and `lastPhraseInterestUi` (`interest N.N`) diagnostics
in the status line, so the threshold can actually be tuned against real playing rather than guessed.

12 new test assertions (136 total, all passing): `computePhraseInterest`'s edge cases (single
note/empty phrase both score 0), and the comparisons that matter - a varied, spread-out phrase scores
meaningfully higher than the identical note count repeated on one pitch, and a maximally rich phrase
(enough notes, full variety, full spread) saturates at exactly 1.0. Rebuilt + reinstalled, Build
~17:31 UTC 2026-09-15.

**RESOLVED**: live-tested successfully - the user reported a cascading 4-track network, each track's
OrchDelay feeding the next (1→2→3→4→1), sustaining itself indefinitely from a single kick-start
motive. Not a designed feature - a genuine emergent result of the responsorial architecture working
across chained instances in a real rig. Moving on to item 4.

### Multi-motive memory bank

Before this item, OrchDelay only ever remembered the single most-recently-closed phrase - each echo
was strictly a LOCAL response to whatever was just played, with no way to develop a recurring idea
across a longer stretch of performance. `odly::MemoryEntry` captures just enough of a past phrase to
echo it again later (its notes plus phraseStart/End, needed by Retrograde/Stretch's own timing math).
The processor keeps a small pool (`phraseMemory`, fixed at `kMaxPhraseMemorySize=8`, oldest evicted
first - not exposed as a parameter in this first version, deliberately: the user's own three
candidate SELECTION strategies from the transcript review - most recent/most repeated/highest-energy
- are a real design space worth its own follow-up rather than guessing at v1) of phrases actually
PLAYED, never a callback substitution itself.

A new `callbackProbability` parameter (0-100%, default 0% = feature fully disabled) is checked once
per closure, inside `scheduleClosedPhrase` (right after the quality gate, before scheduling):
`odly::resolveMemoryCallback` decides whether THIS closure should echo itself or reach back to an
OLDER phrase, and if so, which pool index - uniform at random across the whole pool in this first
version, no weighting toward "most recent" or similar (same explicit v1 scope cut as pool-selection-
strategy above). Critically, **only the musical CONTENT can be swapped - THIS closure's own timing
(`scheduledFirePpq`, already fixed by `closePhrase` before `scheduleClosedPhrase` ever runs) is never
touched**: a callback answers at the SAME "N bars later" moment the current phrase would have, just
with older material. The swap happens by overwriting `notes`/`phraseStartPpq`/`phraseEndPpq` on a
COPY (`toSchedule`) before it reaches `resolveAndScheduleTransform` - the ORIGINAL `closed` phrase (its
own real content) is what gets pushed into the memory pool afterward, never the substituted one, so
a chain of callbacks can't gradually replace the pool with copies of copies. Because the swap happens
before `resolveAndScheduleTransform` runs, both the transform CHOICE (Content-Aware Weighting reads
`toSchedule.notes`) and the transform MATH correctly operate on whichever content actually ends up
scheduled.

New `totalMemoryCallbacksUi` (`callbacks N`) diagnostic in the status line. 8 new test assertions (142
total, all passing): `resolveMemoryCallback`'s edge cases (empty pool never calls back regardless of
probability; 0% probability never calls back regardless of pool size), determinism, index bounds, an
observed ~50% callback rate at a 50% probability setting across a large sample, and index spread
across the whole pool (not clustered). The actual callback substitution mechanics
(copy-then-override-then-push-original) live in the processor and aren't independently unit-tested -
a known gap matching the same category as the stop/rewind transition logic and Overlap Mode's own
busy-gating. Rebuilt + reinstalled, Build ~18:13 UTC 2026-09-15.

**RESOLVED**: live-tested successfully. Moving on to item 5, the last of the "second list."

### Overlay/Ducking capture modes

The last and most architecturally significant item - the only one of the 5 that reopens an already-
settled design decision (SS2/SS3.2's own "live notes are always fully swallowed, only the echo
sounds"). Deliberately built LAST, after the other 4, so nothing else in this session's work depended
on the swallow-only guarantee changing.

New `odly::CaptureMode` (`captureMode` parameter, default `Replace`=0, unchanged from the original
behavior):
- **Replace** - unchanged original behavior. Only the delayed echo ever sounds.
- **Overlay** - the live note ALWAYS passes through immediately, alongside whatever echo may be
  sounding - layered, canon-like coexistence rather than strict alternation.
- **Duck** - the live note passes through only when its pitch falls OUTSIDE the range currently
  spanned by whatever OrchDelay has actually fired and is still sounding
  (`odly::isOutsideActiveRange`, computed from `activeFiredNotes` - an empty list means nothing is
  occupying any register, so everything passes freely). Gives the echo the floor in its own register
  while still letting live playing through elsewhere, rather than either full silence or full overlap.

**Critical in all 3 modes: the live note is ALWAYS still captured into the buffer for its own future
echo, regardless of Capture Mode** - this setting only ever decides whether the SAME note ALSO sounds
immediately, never whether it gets buffered. No change to the capture/close/schedule pipeline that
every other item in this session was built and tested against.

**Stuck-note risk, and how it's avoided**: Duck mode's pass-through decision is evaluated PER NOTE-ON,
against whatever the echo's pitch range happens to be at that instant - but a note held by the
performer could easily still be sounding by the time its OWN note-off arrives, by which point the
echo's range (or whether anything is sounding at all) may have changed. Re-evaluating the SAME
pass-through question at note-off time risks a note whose note-on passed through never getting a
matching note-off (permanently stuck downstream) - exactly the bug class this ecosystem has hit
before (OrchPiano's own `PendingRestrike`/`PendingTremolo` history, cited throughout this repo's own
design decisions). Fixed by tracking, not re-deciding: a new `passthroughHeld[channel][pitch]` table
(processor-owned) is set the moment a note-on passes through; when THAT note's own real note-off
arrives, it passes through UNCONDITIONALLY if the table says so, regardless of what the current range
looks like by then. `drainAndSilence()` - already the single place every other stuck-note risk in
this plugin gets swept (stop, rewind, bypass) - was extended to ALSO flush any still-held
passthrough entries the same way it already flushes `activeFiredNotes`: never clear tracking without
emitting the note-offs in the same operation.

6 new test assertions (148 total, all passing) cover `odly::isOutsideActiveRange` directly (empty-list
edge case, inclusive range boundaries, both sides of the range). The actual pass-through/tracking
mechanics live in the processor (real-time MIDI I/O, not expressible as a pure function) and aren't
unit-tested - the same known-gap category as every other real-time-state mechanism in this plugin
(stop/rewind transitions, Overlap Mode's busy-gating, the memory-bank substitution). Rebuilt +
reinstalled, Build ~18:19 UTC 2026-09-15. **Not yet live-tested** - required before calling this
actually done, per this repo's own established discipline. This closes out all 5 items of the
"second list" (SS18-SS22) pending live confirmation of this last one.

**RESOLVED**: live-tested successfully ("they are working fine").

## SS23. Two-column editor layout - the single column grew too tall

Across this session's feature growth (v1's original 7 controls to v1.1's full second-list expansion),
the editor grew from `520x640` to `520x1076` as a single vertical column - past what fits in a typical
plugin window on the user's own screen; the bottom controls (Instance Seed, Randomize, the status
line) were being clipped with no way to scroll to them. User's own direct request: split it, put the
"lower half" beside the upper half instead of below it.

Restructured `resized()` into a shared full-width header (title/subtitle/build/Bypass) and a
shared full-width status line at the bottom, with everything else split into two side-by-side
columns: **left = capture & timing** (Capture Mode, Hold Bars, Overlap Mode, Phrase Gap, Minimum
Interest, Callback Probability, Instance Seed) and **right = transform** (Restlessness, Transform
choice, and all 5 amount sliders - Transpose/Rotation/Length/Stretch/Interval). Both columns land at
almost exactly the same height (7 rows each, 54px per row) by construction, not by tuning - the
grouping happened to split evenly. Window resized to `1040x680` - shorter than the single-column
peak, and only modestly wider than before. Purely a layout change; no parameter, logic, or test
changes. Rebuilt + reinstalled, Build ~18:44 UTC 2026-09-15.

**RESOLVED**: user confirmed everything landed and is visible.

## SS24. Autonomous Fire - the device can free-run off its own memory

Prompted by the user's own live discovery: a cascading 4-track network (each track's OrchDelay
output feeding the next, 1→2→3→4→1) sustaining itself from a single kick-start motive - but, as the
user put it, "each one has to wait for the feeding phrase, then wait to fire, even when the pool is
full of phrases that could be fired independently of a new material." Correct diagnosis: OrchDelay
was, until this item, 100% reactive - the memory bank (SS21) only ever decides WHAT CONTENT an echo
uses, never WHETHER to fire one at all without a freshly-closed phrase to trigger it. If any single
link in a chained rig goes quiet (a phrase skipped by the quality gate, discarded by Overlap Mode's
Skip, etc.), the whole network could stall, since nothing else would ever prompt the next track.

**Autonomous Fire** (`autonomousFireBars`, 0-16, default 0 = off) lets the device fire from its own
memory pool on its OWN clock, entirely independent of new incoming MIDI, once seeded with at least
one real captured phrase - the "feed each track once, then let it free-run" workflow the user
described directly.

Mechanics (`checkAutonomousFire`, called once per playing block, deliberately OUTSIDE the Hold-
Bars-pause gate - see below): a self-sustaining "every N bars" clock, armed the first time the pool
has ≥1 entry (`autonomousFireArmed`/`nextAutonomousFirePpq`), re-armed fresh (not resumed from a
stale schedule) after a genuine rewind, since "now" moved. Each tick draws a pool index via
`odly::resolveAutonomousFireIndex` - same uniform-random selection philosophy as
`resolveMemoryCallback`'s own draw, but keyed by a dedicated `autonomousFireCounter` (there's no
just-closed phrase to tie a `phraseCounter` value to) and its own salt (13), independent of every
other seeded draw here - then builds and schedules a `Phrase` directly from that pool entry via the
same `resolveAndScheduleTransform` every other closure path already uses, firing essentially
immediately (this tick IS the fire moment, not "N bars from here" - the interval itself already
governs cadence).

**Deliberately independent of Hold Bars' own pause state** (SS17's `Hold Bars=0`): Autonomous Fire
fires from EXISTING pool content, not from newly-captured material, so pausing capture ("stop
listening for new phrases") and Autonomous Fire ("keep echoing what I already have") are treated as
orthogonal controls, not coupled - matches the user's own "seed once, then let it run" framing, where
you'd plausibly want to stop capturing NEW input while the device keeps developing what it already
has.

**Deliberately reuses the existing pipeline rather than adding a parallel one**: an autonomously-fired
phrase becomes a normal `pendingPhrases` entry, so Overlap Mode (SS17), the transform-choice logic
(manual or Content-Aware-Weighted, SS19), and the fire/note-off loops all apply to it exactly as they
would to any other phrase, with zero special-casing anywhere else in the codebase. `phraseCounter`
(shared with real closures, not a separate counter) still drives transform-choice hashing, so
autonomous and real-triggered echoes draw from the same evolving hash sequence rather than two
disconnected ones.

New `totalAutonomousFiresUi` (`autofire N`) diagnostic. Editor gained an "Autonomous Fire (bars)"
slider in the left (capture & timing) column; window height 680→700 to fit (the left column is now
one row taller than the right, no longer perfectly balanced).

5 new test assertions (153 total, all passing): `resolveAutonomousFireIndex`'s edge case (empty pool
→ -1), determinism, index bounds, spread across the whole pool, and independence from
`resolveMemoryCallback`'s own index draw (different salts). The actual clock-arming/ticking mechanics
live in the processor (real-time ppq state) and aren't unit-tested - the same known-gap category as
every other real-time-state mechanism in this plugin. Rebuilt + reinstalled, Build ~21:12 UTC
2026-09-15. **RESOLVED** - user confirmed live: "It works." Two follow-ups came directly out of that
test, both addressed in SS25 below: an empty-pool command (testing new ideas required an app
restart otherwise), and multi-bank memory (this section's own closing note about pool-selection
strategy and section changes). The "shared clock/signal" for inter-instance coordination remains
open, deliberately deferred to its own future conversation (see SS25's own closing note).

## SS25. Clear Bank + multi-bank memory (A/B/C) - a development-section workflow

Directly prompted by the user's own live report right after confirming Autonomous Fire worked: "Yes,
an empty pool command is needed, otherwise it is difficult to try new ideas/motives. And also the
shared clock/signal. My idea was to have 2-3 banks (pools) available, ready to be recalled - like in
a development section, where material from different sections are mixed together. These banks would
be filled just by playing a different midi clip for capture (A/B/C), then recall them after N bars
(an automatable knob would make the switch, or even the same autonomous mechanism that we already
have)."

Two features shipped together here, both self-contained single-instance work. The "shared
clock/signal" for cross-instance coordination is explicitly NOT part of this section - it deserves
its own focused conversation rather than being bolted on here, and remains on the list, not dropped.

**Clear Bank**: a momentary button (`requestClearCaptureBank()`) that empties whichever bank
`captureBank` is currently set to, so a fresh idea can be auditioned without stale material bleeding
back in via Callback Probability or Autonomous Fire. Not a normal APVTS parameter - there's no clean
"momentary trigger" shape in APVTS (same reasoning as the existing `randomizeInstanceSeed`), and
unlike that method (which only ever touches its own atomic parameter float), this one reaches into
`phraseMemoryBanks`, a plain member vector the AUDIO thread also reads/writes every block. A direct
`.clear()` from the message-thread button click would be a real data race. Instead the click just
raises `std::atomic<bool> clearCaptureBankRequested`; `processBlock()` consumes it via
`.exchange(false)` at the very top of the NEXT block, on the audio thread itself, and clears against
`captureBank`'s value read at that moment - the same message-thread/audio-thread separation pattern
this codebase already uses for parameter reads, just applied to a one-shot command instead of a
continuous value.

**Multi-bank memory**: the single `phraseMemory` vector (SS21) became
`std::array<std::vector<odly::MemoryEntry>, 3> phraseMemoryBanks` (banks A/B/C, each still capped at
8 entries, oldest evicted first, exactly as before). Two new `AudioParameterChoice` selectors,
deliberately DECOUPLED per the user's own framing above (fill a new bank while a different one keeps
playing, then switch over) rather than one shared knob:

- **Capture Bank** - which bank a newly-closed phrase is written into (`scheduleClosedPhrase`'s
  push-back/evict-oldest step).
- **Active Bank** - which bank Callback Probability (`scheduleClosedPhrase`'s memory-callback read)
  AND Autonomous Fire (`checkAutonomousFire`) both draw FROM. Deliberately shared between these two
  consumers rather than each getting its own selector - both are "echo something old" mechanisms, and
  splitting them would let the pool a track is currently developing from silently fork into two,
  with no clear musical rationale for why they'd ever differ.

No new `odly::` pure functions were needed: `resolveMemoryCallback`/`resolveAutonomousFireIndex`
already take `poolSize` as a plain int, agnostic to which bank the caller passes in, so both existing
salt-11/salt-13 hash draws carry over unchanged - the bank selection itself is a plain deterministic
parameter read, not a randomized draw, so no new salt was needed either.

Editor gained "Capture Bank" (ComboBox + "Clear Bank" button on the same row) and "Active Bank"
(ComboBox) rows in the left (capture & timing) column, placed after Autonomous Fire and before
Instance Seed; window height 700→760 to fit (left column now 2 rows taller than before).

No new tests were strictly required at the pure-logic layer (no new `odly::` functions), so the
existing 153-assertion suite is the full coverage here - confirmed still passing after this change.
Rebuilt + reinstalled, Build ~2026-09-16. **RESOLVED** - user confirmed live: "All confirmed working."
Pushed to `github.com/johnpascu77-dotcom/OrchDelay` (`df99c8f..8e7ef3c`) per explicit request even
before testing ("push it anyway... I am confident that it will work as expected"), then verified the
next session. Recency-weighted pool selection, flagged as a follow-up here, is addressed next in
SS26.

## SS26. Recency Bias - weighting the memory pool draw toward newer material

User's own stated priority ordering for the two items left open after SS25 ("First 1, then 2"): this
recency-weighting item first, the cross-instance shared-clock/signal item second (still deliberately
deferred to its own conversation, not started here).

Until now, both consumers of the memory pool - Callback Probability's occasional "echo something
older instead" and Autonomous Fire's own free-running draw - picked uniformly at random across
whichever bank Active Bank points to, with no notion of *how old* an entry was. **Recency Bias**
(`recencyBias`, 0-100%, default 0%) lets either mechanism instead favor more RECENTLY captured
material - useful once a bank has built up a few generations of phrases and newer material should
dominate without discarding older entries outright (which Clear Bank already handles for the "start
completely fresh" case).

New shared helper `odly::resolveRecencyWeightedPoolIndex(instanceSeed, counter, salt, poolSize,
recencyBias)`: pool index 0 is assumed OLDEST, `poolSize-1` NEWEST (matching `phraseMemoryBanks`' own
FIFO-evict-oldest ordering - the oldest entry is always erased from the front). Weight for index `i`
is `(i+1)^exponent`, where `exponent` runs from 0 at `recencyBias=0` (every index weighted equally -
flat/uniform, i.e. the function degrades gracefully to the original behavior on its own terms) up to
4 at `recencyBias=1` (strongly favors the newest few entries); a single `hashUnit` draw then walks the
normalized cumulative weight sum to pick one index - the exact same "target-then-walk-the-cumulative-
sum" pattern `proposeWeightedTransform`'s own salt-10 content-aware draw already uses (Docs SS19), not
a new pattern invented for this feature.

**Zero behavior change at the default (0%)**: rather than routing every draw through the new weighted
function unconditionally, `resolveMemoryCallback` and `resolveAutonomousFireIndex` both keep their
ORIGINAL mod-hash uniform-pick code path completely untouched for `recencyBias<=0`, and only call into
`resolveRecencyWeightedPoolIndex` above that threshold. This matters because the weighted function's
own draw sequence, even at bias=0, is not bit-for-bit identical to the original mod-hash sequence
(same statistical uniformity, different formula) - existing sessions that never touch this new knob
must see the literal, unchanged draw sequence they've always gotten, not just a statistically similar
one. Two new salts: 14 for the memory-callback weighted draw, 15 for the autonomous-fire weighted
draw - independent of each other and of every other seeded draw in this codebase, same convention as
every prior salt addition.

Editor gained a "Recency Bias" slider in the left (capture & timing) column, placed after Active Bank
and before Instance Seed; window height 760→815 to fit (left column now 3 rows taller than the right).

9 new test assertions (162 total, all passing): the shared helper's own edge cases (empty pool → -1,
single-entry pool → 0), determinism, bounds, bias=0 spreads roughly evenly (regression guard against
this function itself silently becoming skewed even before any caller uses it), bias=1 clearly favors
the newest entries, and salt-14-vs-15 independence; plus `resolveMemoryCallback`/
`resolveAutonomousFireIndex` each gained one new assertion confirming `recencyBias=1` visibly shifts
their own draw far toward the newest pool entries (roughly 3x+ as many high-index as low-index draws
across 4000 trials), on top of their existing bias=0 (untouched, still uniform) coverage. Rebuilt +
reinstalled, Build ~2026-09-16. **RESOLVED** - live-tested by the user next session ("All confirmed
working"), together with SS25 (Autonomous Fire + multi-bank/Clear Bank) - see SS25's own resolved
note above.

## SS27. Cross-instance phrase broadcast (Remote bank) - item 2 of the user's own priority list

Directly continues the "shared clock/signal" thread opened alongside the multi-bank request (SS25):
the user's own words there were "And also the shared clock/signal... I am also thinking of a way to
make them communicate to each other." After SS25/SS26 shipped and were confirmed working, the user
gave an explicit priority order for what remained - "First 1, then 2" (1 = recency-weighted pool
selection, SS26; 2 = this item). I laid out three concrete interpretations of "shared clock/signal"
before building anything (phase-aligning independent Autonomous Fire clocks; letting instances feed
each other's memory pools directly without a MIDI cable; a designed canon/round firing relationship)
and the user chose option 2 outright: direct cross-instance pool sharing.

**Design grounded in real ecosystem precedent, not invented from scratch.** Before writing any code,
I checked whether any existing Orch plugin already does in-process cross-instance C++ state sharing -
none do. The closest precedent is OrchMerge's and OrchCapture's own loopback-TCP link objects
(`OrchMergeLink`/`OrchCaptureLink`): one instance elects itself Hub/Coordinator via a manual parameter
toggle, binds a fixed port with `juce::InterprocessConnectionServer`, and announces itself via a lock
file in the temp directory; every other instance polls for that lock file and, once found, connects
as a Client via `juce::InterprocessConnection`. All socket work happens on the link object's OWN
background `juce::Thread`, never the audio thread or the host message thread - OrchCaptureLink's own
devlog records that an earlier version driven by a `juce::Timer` (implicitly on the message thread)
froze Bitwig outright with ~50-100 instances in the rig. `OrchDelayLink` (new files
`Source/OrchDelayLink.h/.cpp`) follows this exact same shape, adapted for a genuinely different
topology.

**Where OrchDelay's needs differ from the precedent**: OrchCapture/OrchMerge are asymmetric
(Coordinator aggregates; Clients only ever push, never receive anything back). OrchDelay needs a true
symmetric pub/sub relay - ANY instance may both publish (Broadcast Channel > 0) and subscribe (Listen
Channel > 0) at the same time, regardless of whether it happens to be the elected hub. The hub's own
extra job beyond being a normal peer is a dumb fan-out relay: any phrase it receives from one client
gets forwarded, unmodified, to every OTHER connected client - it never filters by channel itself, each
client decides locally whether an incoming phrase's channel matches its own Listen Channel. The hub
also applies an incoming phrase to its own Remote bank when ITS OWN Listen Channel matches (it's a
peer, not exempted from listening) - but a hub's own LOCAL publish is never looped back into its own
Remote bank (Remote is exclusively for material from OTHER instances; a hub echoing its own broadcast
to itself would be redundant with its own A/B/C banks and just confusing). This topology also turns
out to have no possible "hear my own echo" case by construction: a client's own publish only ever
travels TO the hub, and the hub's relay explicitly excludes the sender's own connection from the
fan-out - so no `src`/instance-uid tagging was needed at all to prevent self-hearing, simplifying the
wire protocol.

**New parameters**: `linkHub` (bool, default off - exactly ONE instance in the whole rig should have
this on, same manual-designation convention as OrchCapture's Coordinator/OrchMerge's Hub, not
automatic election), `broadcastChannel` (0-8, default 0=off - which channel this instance's own
captured phrases publish to), `listenChannel` (0-8, default 0=off - which channel this instance
receives OTHER instances' phrases from). All three default off, so an instance with this feature
untouched behaves identically to before.

**Why absolute host ppq needs no translation**: every OrchDelay instance in a Bitwig project already
reads the SAME host transport (same bpm, same ppq, same time signature) - this was already true before
this feature (it's how Autonomous Fire's own bar-boundary math stays correct per-instance). A phrase's
`phraseStartPpq`/`phraseEndPpq`, captured on one instance and later scheduled/fired from a completely
different instance's Remote bank, is therefore directly meaningful on arrival with zero adjustment -
this is what actually makes direct pool-sharing tractable at all, rather than needing some translation
or re-basing scheme.

**Remote bank**: `phraseMemoryBanks` grew from 3 slots (A/B/C) to 4 (`kPhraseMemoryBankCount = 4`),
index 3 (`kRemoteBankIndex`) reserved for phrases received over the link. Capture Bank's own valid
range is now `kCaptureBankChoiceCount = 3` (A/B/C only - Remote is never a valid CAPTURE target, it's
populated exclusively by incoming broadcasts) while Active Bank widened to all 4 choices
("A"/"B"/"C"/"Remote"), so Callback Probability and Autonomous Fire can both be pointed at received
material exactly like any local bank. Remote gained its own dedicated "Clear Remote" button
(`requestClearRemoteBank`) rather than overloading Capture Bank's own selector with a 4th choice that
could never actually be captured into.

**Never trust another instance's own `seq` numbering**: `odly::memoryEntryToVar`/`memoryEntryFromVar`
(new, tested via an actual JSON-string round-trip, not just a `juce::var`-tree round-trip) deliberately
drop `HeldNote::seq`/`phraseId`/`hasNoteOff` on the wire - only pitch/velocity/channel/onset/duration
plus the phrase's own start/end cross the link. `seq` in particular was assigned by a DIFFERENT
instance's own independent monotonic counter and could numerically collide with THIS instance's own
currently-active notes if reused verbatim - the exact bug shape (`(channel,pitch)`/id-alone matching
collisions) OrchPiano's own devlog documents hitting three separate times. The receiving instance
reassigns fresh local `seq` values from its own `nextNoteSeq` counter at the moment a received phrase
is folded into the Remote bank (top of `processBlock`), never at any earlier point.

**Thread-safety discipline - two independent small mutex-guarded handoffs, one per direction, both
following OrchMergeLink's own established non-blocking-from-the-audio-thread rule (`try_lock`; skip
and retry next block on contention, never block real-time processing for IPC's sake)**:
- OUTGOING: `scheduleClosedPhrase` bumps `capturedGenerationUi` (unconditionally, cheap) and
  `try_lock`-copies the just-captured entry into `lastCapturedEntry` every time ANY phrase closes.
  `OrchDelayLink`'s own worker thread polls `getCapturedGenerationForUi()` once per `kPollMs` (300ms),
  and on a change, reads the snapshot via `snapshotLastCapturedForBroadcast()` (a BLOCKING lock there -
  safe, since that call only ever happens on the link's own thread, never the audio thread) and
  publishes it if Broadcast Channel > 0.
- INCOMING: `pushIncomingRemotePhrase` (called from the link's own connection thread, blocking lock
  fine there too) appends to `remoteInboxPending`; the top of `processBlock` drains it via `try_lock`,
  reassigning fresh `seq` values and folding each entry into the Remote bank (oldest evicted first,
  same 8-entry cap as every other bank), incrementing new UI counter `totalRemoteReceivedUi`.

**Editor**: new "Broadcast Hub" toggle + "Clear Remote" button (one row), "Broadcast Channel" and
"Listen Channel" sliders, added to the RIGHT (transform) column after Interval Scale - the left
(capture & timing) column was already the taller of the two (11 rows vs. the right's 7 before this
change), so the new cluster went to the shorter column to keep the two columns closer in height rather
than widening the gap further. Status line gained a link-mode summary
(`hub`/`hub(busy)`/`client(ok)`/`client(--)`) plus a running `remoteIn` counter.

**Window-height clipping bug, found across two rounds of the user's own live screenshots immediately
after this section's first install**:

Round 1: I had claimed above that no window-height change was needed since the right column's new
10-row total still fit under the left column's own 11-row height - but that reasoning never actually
verified the LEFT column itself still fit inside the window height carried over unchanged from SS26
(815, sized for the left column BEFORE this session added `remoteIn`'s own status-line row, which grew
the status area 68→82px, eating into the same budget). The real per-row arithmetic (header zone 132px +
status zone 94px + top/bottom margins 32px + left column's 11 rows × 54px each = 852px minimum) showed
815 was already ~37px short - confirmed exactly: Instance Seed's own label was the last thing visible,
its slider and Randomize button pushed off the bottom edge entirely. First fix attempt bumped the
window to 1040x890 using the real per-row arithmetic instead of an approximate bump.

Round 2: that fix made Instance Seed itself fully visible, but the user's NEXT screenshot showed the
status label now mostly missing too - only its first ~2 lines visible, with a large visibly-empty gap
of background between Instance Seed and the sliver of status text that did render, i.e. genuinely
"there is enough space" as the user put it, not a real shortage this time. Root cause: `statusArea` was
still computed via `area.removeFromBottom(82)` against the window's own DECLARED height (890) - correct
arithmetic in isolation, but Bitwig's own device panel can render LESS vertical space than a plugin's
declared editor size with no scrollbar and no error, silently cropping whatever falls below its own
panel height. A status label pinned near y=890 landed mostly past whatever Bitwig actually chose to
show, even though the real content (all the rows) ended well above that same visible edge.

**Real fix**: stopped pinning status to the window's own bottom edge entirely. `fullWidthArea` is now
captured before the two-column split (for status's own X/width later); after all rows in both columns
are placed, `contentBottom = max(leftArea.getY(), rightArea.getY())` finds where the TALLER column's
own cursor actually ended, and status is placed directly below THAT (`contentBottom + 8`), never
against a number derived from the declared window height. This makes the layout self-correcting for
future row additions or host-panel-height quirks: status always appears immediately after real content,
regardless of how much (or little) of the declared window a given host actually renders. Window height
simplified back down to 1040x860 (a modest margin over the still-useful ~848px real minimum, now purely
a courtesy for hosts that DO render the full declared size, not a load-bearing number status placement
depends on). Lesson for this window's own history: three straight rounds of clipping bugs (SS23's
original single-column overflow, this section's two rounds) all trace back to the same root mistake -
computing a layout number by hand instead of deriving it from where content actually ends, or (this
round) anchoring a status label to a host-controlled declared size a host is free to under-render.

7 new test assertions (169 total, all passing) cover `memoryEntryToVar`/`memoryEntryFromVar`'s own
round-trip fidelity through an ACTUAL JSON string (not just a `juce::var` tree, which could hide a real
wire-format bug) and confirm `seq` is deliberately NOT preserved. The IPC/threading machinery itself
(`OrchDelayLink`, socket lifecycle, hub election, relay fan-out) is not unit-tested - same known-gap
category as every other real-time/host-integration mechanism in this plugin (Autonomous Fire's own
clock-arming logic, the transport-polling code, etc.) - it needs a real 2-instance Bitwig test.

Rebuilt + reinstalled, Build ~2026-09-16. **Not yet live-tested** - required before calling this
actually done, per this repo's own established discipline; this is also the first feature in the whole
plugin that has never been exercised with two ACTUAL separate plugin instances talking to each other,
so the live test matters more than usual here. Phase-aligning independent Autonomous Fire clocks
(interpretation 1 from this section's own opening) and a designed canon/round firing relationship
(interpretation 3) remain unbuilt, un-requested follow-ups if the user ever wants to revisit them.

**Bug found before any live IPC test even happened**: walking the user through the exact 3-instance
setup (1 Hub + 2 Listeners) surfaced that the "Remote" choice this section added to `activeBank` was
completely invisible in the actual Active Bank dropdown - only "A"/"B"/"C" ever showed. Root cause: the
`AudioParameterChoice` itself was correctly widened to 4 choices in `createParameterLayout()`, but the
corresponding `activeBankBox` `ComboBox` in the editor still only had 3 `addItem()` calls left over from
before this section's own change - `ComboBoxAttachment` maps ComboBox item IDs 1..N directly onto the
AudioParameterChoice's own choice indices 0..N-1, so a ComboBox with fewer items than its backing
parameter simply makes the extra choice(s) unreachable through the UI, even though the parameter itself
supports them (automation/state could still reach index 3, just never a mouse click). Fixed by adding
the missing `activeBankBox.addItem ("Remote", 4);`. A follow-up audit of all 5 ComboBoxes in the editor
(`captureModeBox`/`overlapModeBox`/`captureBankBox`/`activeBankBox`/`transformBox`) against their own
backing `AudioParameterChoice` `StringArray`s confirmed this was an isolated slip, not a systemic
pattern - every other ComboBox's item list already matched its parameter's choice list exactly (count,
text, and order). Rebuilt + reinstalled.

## SS28. Autonomous Fire drift/jitter - a real bug in the re-arm math and the fire anchor

Reported live: "I begin to feel the need to correct the Autonomous Fire phase drift, because I get a
'jitterish' effect soon after the start, that sounds more like a latency problem, notes landing
sometimes too close to each other (not in a musical way - e.g. 3 against 2 - rather a 32nd or 64th
apart)." This turned out to be a genuine bug in `checkAutonomousFire` itself (SS24), not a case for the
"phase-aligning independent Autonomous Fire clocks" idea floated (but not built) in SS27 - this fix
benefits a SINGLE instance's own cadence regardless of whether any other instance exists.

**Two compounding root causes, both in the original SS24 code**:

1. **Drift**: re-arming used `nextAutonomousFirePpq = blockEndPpq + intervalPpq` - rebased from
   "whenever the triggering block happened to end," not from the PREVIOUS ideal due time. Since
   `blockEndPpq` always overshoots the true due moment by up to one block's worth of ppq (block
   granularity, typically a few milliseconds), and every cycle re-derives its own next due time from
   that same overshot number, the error compounds every single cycle - explaining "soon after the
   start": the first tick or two are close to correct, then it creeps progressively later relative to
   a clean N-bar grid.
2. **Jitter**: the actual fire was anchored to `autoPhrase.scheduledFirePpq = blockStartPpq` (the
   triggering block's own start) rather than the precise due ppq (`nextAutonomousFirePpq` itself,
   which was already known and >= the true due moment by definition of the "due" check). Since the
   per-note fire loop's sample-offset math is `(note.outputOnsetPpq - blockStartPpq) / ppqPerSample`
   clamped to `[0, numSamples-1]`, anchoring to `blockStartPpq` forces `outputOnsetPpq == blockStartPpq`
   for the first note of every single autonomous fire, which maps to EXACTLY sample 0 of whatever block
   noticed it was due - throwing away the sample-accurate precision every other note in this plugin
   already gets, and landing at a coarse, block-quantized position that can fall unexpectedly close
   (a 32nd/64th note, matching the user's own description) to some other already-scheduled note whose
   own true position isn't quantized the same way.

**Fix**: new pure function `odly::resolveNextAutonomousFirePpq(previousDuePpq, intervalPpq,
blockEndPpq)` - advances from the PREVIOUS ideal due time by whole multiples of `intervalPpq`, walking
forward past `blockEndPpq` (a `while` loop, not a single `+=`, so falling far behind after an unusually
long block or a pause catches up to the first still-future grid point rather than firing a burst of
catch-up ticks one per block). `checkAutonomousFire` now captures `const double firePpq =
nextAutonomousFirePpq` (the precise due ppq) BEFORE re-arming, uses it as `autoPhrase.scheduledFirePpq`
(not `blockStartPpq`), and re-arms via `nextAutonomousFirePpq =
odly::resolveNextAutonomousFirePpq(firePpq, intervalPpq, blockEndPpq)` (not `blockEndPpq +
intervalPpq`). Together: the cadence stays perfectly grid-locked forever (zero accumulated drift), and
each fire lands at its own true sample-accurate position instead of a block-quantized one.
`checkAutonomousFire`'s own `blockStartPpq` parameter became dead code after this fix and was removed
from its signature (processor .h/.cpp and the one call site in `processBlock`).

3 new test assertions (172 total, all passing): `resolveNextAutonomousFirePpq` advances by exactly one
interval from the previous due time regardless of how much `blockEndPpq` overshot it (the literal
regression test for root cause 1); stays perfectly grid-locked across 200 simulated cycles of varying
block-boundary slop, asserting the EXACT expected ppq each cycle rather than just "close enough"; and
catches up correctly (lands on the first still-future grid point, not merely one interval ahead) after
falling more than a whole interval behind. The real-time clock-arming mechanics around it (whether
`checkAutonomousFire` gets called at the right moments, `autonomousFireArmed`'s own state machine)
remain in the processor and untested by the console app, same known-gap category as always - this fix
targets exactly the pure-math piece that was actually wrong.

Rebuilt + reinstalled, Build ~2026-09-16. **Not yet live-tested** - required before calling this
actually done, per this repo's own established discipline.

## SS29. Wait-release drift - the same anti-pattern class as SS28, found in a different spot

Reported live, after SS28's fix was already installed: "Better overall. No more 'particles', but we can
still see the drifting towards the ending of phrases, per track, like very small values are slowly
adding up with each note." First hypothesis - different random Stretch draws per phrase producing
apparent drift - was directly challenged and correctly ruled out by the user: "we said that even in
Random mode, Stretch would apply only consacrated binary or ternary values, not 'in-betweens'." A real
inter-onset-interval pass over the reported take confirmed this: the ratios clustered tightly on clean,
unstretched values (0.25/0.33/0.5/~0.7/1.0/1.5/2.0 of a quarter) across the whole take, not on a mix of
random Stretch ratios - ruling out Stretch variance as the driver and pointing back at the scheduler
itself, the same place SS28's bug lived.

**Root cause, architecturally identical to SS28's jitter cause**: Overlap Mode "Wait" (see SS17), when
releasing a phrase that had been held because `activeFiredNotes` was non-empty, anchored the release to
`blockStartPpq - phrase.outputNotes.front().outputOnsetPpq` rather than to the precise moment the device
actually became free. Worse than SS28's single-cause version, this one had two layers:

1. **Stale busy-state**: the note-off removal loop (clearing `activeFiredNotes` for notes whose own off
   had arrived) ran AFTER the phrase fire/release loop in the same `processBlock` call. So a note that
   finished partway through the current block still counted as "active" for this block's busy check -
   release could only happen on the FOLLOWING block, a full block later than the device actually freed
   up.
2. **Coarse anchor**: even once released, the shift was anchored to that following block's own
   `blockStartPpq` (block-granular) rather than the actual ppq the device cleared - the same
   `blockStartPpq`-as-"now" anti-pattern SS28 already fixed for Autonomous Fire, just in the Wait-release
   path instead of the re-arm path.

Both layers push the release strictly later, never earlier, and `odly::shiftOutputNotes` then carries
that lateness through the phrase's ENTIRE remaining note schedule (correct behavior in itself -
preserving the phrase's own internal rhythm - but it means the error is baked into every later note of
that release, not just the first). Because a late release also delays when the device next reads as
free, each Wait-release can nudge the NEXT queued release later still - a genuine compounding effect
that grows with the NUMBER of Wait-release events over a take, not with elapsed time. That matches "very
small values slowly adding up with each note" exactly, and explains why it showed up specifically
"towards the ending of phrases" once a take had accumulated enough busy periods.

**Fix**: the note-off removal loop was moved to run BEFORE the phrase fire/release loop within the same
block (fixing layer 1 - a note-off that lands inside the current block is now cleared from
`activeFiredNotes` before the busy check runs), and it now records `justClearedPpq`, the precise ppq of
whatever note-off it just processed. The Wait-release anchor became `releasePpq = juce::jmax
(blockStartPpq, justClearedPpq)` instead of bare `blockStartPpq` (fixing layer 2 - when the device frees
up mid-block, the release anchors to that precise moment rather than rounding up to the block boundary).
No new pure function was needed (unlike SS28's `resolveNextAutonomousFirePpq`) - this was purely a
processor-level reordering plus a more precise anchor value, so no new `odly::` test coverage was added;
`shiftOutputNotes` itself (already tested) was untouched.

Rebuilt + reinstalled, Build ~2026-09-16. **Not yet live-tested** - required before calling this actually
done.

**On the "Wait" naming challenge** - also raised in the same report: "If 'Wait' is not waiting the
previous phrase to end, then why we called it this way? ... this seems more like Overlapping." This is a
fair challenge to the NAME, and worth being direct about rather than talking around it. Overlap Mode has
only ever governed one thing (see SS17 and the comment at the top of the fire/release loop in
`processBlock`): whether a phrase's own FIRST note may start while an EARLIER PHRASE's notes are still
sounding. It has never gated a phrase's own LATER notes against its own busy state, and it has never
touched a single phrase's own internal texture - "a Stretched phrase's own notes overlapping each other
is left alone" is a deliberate, pre-existing design choice (not something changed by this fix). So the
"polyphonic overlapping" reported separately from the SAME session, on the short Hub-solo take, is very
likely that exact mechanism: a single phrase's own Stretch-lengthened notes overlapping themselves, which
"Wait" was never designed to prevent - not a Wait-gating failure between two different phrases. The
user explicitly declined the "monophonic" fix (clip/shorten a phrase's own overlapping notes) as "too
experimental (hence motive-killer)," so this stays as documented, known behavior rather than something to
change: Wait prevents a NEW phrase from starting over an OLD one's still-sounding notes; it was never a
promise that any one phrase's own material stays monophonic against itself.

## SS31. Connection Matrix - a live "who feeds whom" view for a multi-instance rig

Grew out of debugging a 4-instance cascade setup live with the user (§ before this): the real cause of one
link going silent turned out to be that broadcasting only ever fires from genuinely NEW live-captured
closures (`scheduleClosedPhrase`), never from Autonomous-Fire replays or Remote-bank material - so an
intermediate instance with no live MIDI of its own has nothing to relay onward, even with perfectly correct
channel wiring. Once that was resolved, the user asked (purely conceptually, comparing to AUM's routing
matrix) for a way to see the whole rig's wiring at a glance rather than reading each instance's own
Broadcast/Listen Channel sliders one at a time - "in a bigger orchestral rig, it is very easy to lose
track of who is doing what." Confirmed the real topology is genuinely many-to-many at the RIG level
(several "generator" instances, each fanning out to several "follower" instances) even though each
individual instance only ever has ONE Broadcast Channel and ONE Listen Channel - many-to-many falls out
of several followers independently choosing the same generator's channel number, no new routing
capability needed, just visibility into what's already there.

**Design choice: Instance x Instance, not Channel x Channel.** Rows = every known instance as a potential
source (using its OWN Broadcast Channel), columns = the same instances as potential destinations (using
their OWN Listen Channel), a cell lights up where they match. This reads directly as "row feeds column"
using real instance names rather than bare channel numbers, and it surfaces real configuration hazards for
free: an all-dark row (broadcasting to nobody), an all-dark column (a starved follower), or - flagged in a
distinct warning colour - a column lit by MORE than one row, meaning two different generators are
accidentally reusing the same channel number and a follower is silently receiving interleaved material
from both.

**Read-only for v1, not click-to-wire.** The user's own framing ("make all the connections from one
matrix") pointed at AUM's fully interactive matrix, but that's a materially bigger feature - it means the
hub reaching into ANOTHER instance's own parameters over the network, which is a genuinely new trust
boundary nothing in this plugin family has done before (Link has only ever moved musical content one
direction). Scoped this session to the read-only view; click-to-wire is a real, deliberate follow-up, not
a defaults-to-declined idea - see the "not yet built" list below.

**New wire protocol - a heartbeat, separate from phrase messages**: `OrchDelayLink` messages now carry a
`t` field (`"phrase"`, existing; `"heartbeat"`, new) so the two never get parsed as each other.
`serviceHeartbeat()` (client role only - a hub never needs to hear itself, its own editor reads its own
state directly) sends `{t:"heartbeat", label, bc, lc}` to the hub every poll (300ms), regardless of
whether Broadcast/Listen Channel are actually set - an unconfigured instance should still be VISIBLE in
the matrix, not silently absent, so a forgotten instance is obvious rather than invisible. On the hub side,
`onHubClientMessage` branches on `t`: heartbeats are consumed into a new `std::map<HubConnection*,
RemoteInstanceStatus> remoteStatuses` (guarded by the existing `connectionsMutex`, erased alongside its
matching `serverConnections` entry in `onHubClientGone` - same connection lifecycle, one less thing to
keep in sync separately) and never fanned out (no other client has any use for another's heartbeat, only
the hub's own matrix does); phrase messages are unaffected, still fanned out exactly as before. New public
`OrchDelayLink::getRemoteStatusesForUi()` returns a thread-safe snapshot for the editor's Timer to poll.

**Instance Label**: free-text identity (e.g. "Violins"), shown in the matrix instead of a bare Instance
Seed number. Not an APVTS parameter - no clean free-text parameter shape exists in APVTS, same reasoning
as `randomizeInstanceSeed`'s own non-parameter Randomize button - so it's a plain `juce::String` on the
processor, guarded by its own small mutex (read every heartbeat by the Link's worker thread, written by
the editor whenever the user types), persisted as a plain XML attribute alongside the APVTS state in
get/setStateInformation (a standard JUCE trick for a value that doesn't fit the parameter system). Editor
uses a plain `juce::TextEditor` (no attachment), committing on focus-loss/Return, matching OrchCapture's
own free-text field convention (markers/tempo/score order) rather than firing on every keystroke.

**Editor**: a "Connection Matrix" button appears directly below Listen Channel, but ONLY when Broadcast
Hub is checked (a non-hub instance never receives client connections at all, so it would only ever show
its own single row - matches the user's own "gets active if the Hub is checked" framing). Toggling it
swaps the ENTIRE normal parameter panel for a new `ConnectionMatrixView` component filling the exact same
window real estate (no resize) - implemented by grouping every existing control into one
`mainPanelComponents` vector and toggling visibility on the whole group at once, rather than restructuring
the layout into a real tabbed container. If Hub gets unchecked while the matrix happens to be showing,
`timerCallback` forces it back to the normal view automatically.

Built+installed, verified visually via Standalone (single-instance self-row renders correctly: live label/
channel values, legend, no-crash empty-remote-list case). **The actual cross-instance heartbeat round-trip
is NOT yet live-tested** - needs a real multi-instance rig (or two Standalone instances on the same
machine) to confirm a second instance's heartbeat actually arrives at the hub and renders as a second row.

**Not yet built / deliberately deferred**:
- Click-to-wire (setting a follower's Listen Channel by clicking a matrix cell) - the bigger, riskier
  feature discussed above; build only with a deliberate design pass on the remote-control trust boundary.
- Any hop-count/origin-tag protection for a genuine relay/re-broadcast feature (a SEPARATE idea raised in
  the same conversation, before the matrix - re-broadcasting Remote-drawn/Autonomous-Fire material outward,
  not just first-hand captures) - not built this session, no code changes for it.

**SS31 addendum - 3 real usability findings from the FIRST look at the matrix (same session)**:

1. **No way back.** `matrixTabButton` originally lived down near Listen Channel, inside the exact same
   area `matrixView` covers once shown - and `matrixView`, added as a child AFTER the button (so it draws
   on top), fully occluded and ate its clicks the moment the matrix opened. There was no way back to the
   main view short of unchecking Broadcast Hub. Fixed by moving the button onto the Bypass row, ABOVE
   where `fullWidthArea`/`matrixView`'s own bounds are even computed - structurally impossible to overlap
   again, not just repositioned. Its own text now flips between "Connection Matrix" and "< Back to Main"
   in `setShowingMatrix` so its purpose is always legible.
2. **User instinctively clicked the cells**, expecting them to wire up a connection - direct, valuable
   confirmation that click-to-wire (already flagged above as a deliberate follow-up, not a declined idea)
   is genuinely the expected interaction, not a nice-to-have. Left it read-only for now per the trust-
   boundary reasoning above, but the bottom legend now says so explicitly ("Read-only for now...") instead
   of leaving it to be discovered by a click that does nothing.
3. **Stretch's Random-mode boundaries were invisible.** User had been trying to replicate them with an
   external Bitwig LFO automating the same parameter, going only on the automation knob's own visual
   position - "I had only the optical approximation... I don't know the exact corresponding values."
   `odly::resolveRandomStretchPercent`/`resolveRandomQuantizedStretchPercent` already fully determine the
   real draw range from the slider's own current value alone (`lo = min(100, bound)`, `hi = max(100,
   bound)` - always anchored at 100%, never a fixed-width window around some other centre), so this needed
   no new backend logic - just surfacing it. `stretchLabel`'s own text now reads "Stretch (%) - range
   50.0% to 100.0%" (appending "(quantized)" when Quantize is also on), recomputed live in `timerCallback`
   from `stretchSlider`/`stretchRandomButton`/`stretchQuantizedButton`'s own current UI state - the same
   numbers an external LFO's own min/max would need to match exactly.

Rebuilt+reinstalled, verified visually via Standalone (button reachable and round-trips correctly between
views; Stretch range label updates live and survives a matrix round-trip). Build ~2026-09-17.

## SS30. Wait busy-gating was instant-by-instant, not phrase-envelope-aware - genuine cross-phrase overlap

Follow-up to SS29, same session: the user asked, with a concrete example, how a deliberately simple
monophonic clip (a single hand-built Bitwig MIDI clip, Content-Scaled 200% - i.e. simply doubled in
length, nothing OrchDelay's own Stretch touches) could come out of a SOLO Hub instance with genuine
polyphonic overlap "after a few iterations." Checked the math first: `applyStretch` scales a phrase's
onset-spacing and each note's duration by the SAME factor relative to phrase start, so the overlap
condition (`duration > gap`) is invariant under uniform scaling - stretch, Bitwig's or OrchDelay's own,
mathematically cannot turn non-overlapping material into overlapping material. Confirmed against a real
capture: the take starts genuinely clean (13 notes, zero overlap, including real rests between notes),
then a SECOND independent note stream starts landing on top of the first partway through.

**Root cause**: `checkAutonomousFire` creates and enqueues a new phrase strictly on its own fixed
interval clock, with no awareness of what's currently playing - by design, it relies entirely on the
shared Wait-gate downstream to hold it back if the device is busy (see that function's own doc comment).
But "busy," before this fix, meant `!activeFiredNotes.empty()` - literally "is any note audibly sounding
THIS INSTANT," not "is there a phrase still in progress." A genuinely monophonic phrase almost always has
small rests between its own notes. During any such rest, `activeFiredNotes` reads empty even though the
phrase hasn't actually finished - just paused - and a freshly-due Autonomous Fire phrase slips through
the gate and starts in that gap. When the first phrase's own next note then comes due, it fires anyway
(a started phrase is never re-gated against busy state, by design - see SS29's comment on this), landing
on top of the second phrase. Two perfectly monophonic phrases individually, genuine polyphony as a pair -
despite Wait being active the whole time.

**Fix**: new member `busyUntilPpq` (`OrchDelayProcessor.h`) replaces `activeFiredNotes.empty()` as the
Wait/Skip busy signal. The moment ANY phrase becomes due and starts (Overlap Mode phrases too, though
they never check it themselves), the loop extends `busyUntilPpq` to the max `outputOffPpq` across that
phrase's ENTIRE `outputNotes` - its own full envelope, first note to last, spanning its own internal
rests - not just whichever note is currently sounding. The busy check became `busyUntilPpq >=
blockEndPpq` (still busy through the whole block - stay queued) vs. releasing with `releasePpq =
max(blockStartPpq, busyUntilPpq)` (same precise-anchor principle as SS29, just reading the more accurate
signal now). `busyUntilPpq` is reset to `-1.0` at every point `activeFiredNotes` already gets cleared
(`prepareToPlay`, `drainAndSilence`, `setStateInformation`) - same reset discipline, new variable.
`activeFiredNotes` itself is unchanged in role - still the correct per-note note-off bookkeeping table,
just no longer doubling as the gating signal. The note-off-emission loop, moved earlier in the block for
SS29's sake, moved back to its original end-of-block position now that gating no longer depends on it -
that reordering was a stepping stone superseded by this fix, not a needed permanent change.

No new `odly::` pure function - purely processor-level state tracking, so no new test coverage (172
existing assertions unaffected, confirmed passing). Rebuilt + reinstalled, Build ~2026-09-17. **Not yet
live-tested.**

## SS32. Two-handle min/max Random ranges - all 6 Random-capable parameters, replacing the old single-bound model

User asked directly, after the Stretch-range-label fix (see SS31's own addendum above): "I was imagining
the Stretch Random boundaries as two handles on the slider (min and max)... Equally useful for the other
parameters. Is this possible?" - genuinely different from what shipped moments earlier: the OLD model
always anchored one end of the Random draw at a fixed point (100% for Stretch/Interval, 0 for Transpose/
Rotation, 1 for Length/Hold Bars) and used the single manual slider as the OTHER bound; the user wants two
fully independent bounds with no forced anchor (e.g. Stretch constrained to 110%-130%, never touching 100%
at all). Asked which of the 6 Random-capable parameters (Hold Bars, Transpose, Rotation, Length, Stretch,
Interval Scale) should get this - user chose all 6, for consistency.

**Backend**: every `odly::resolveRandom*` function's signature changed from `(seed, counter, bound)` to
`(seed, counter, minValue, maxValue)` - no longer computing `lo`/`hi` from a single value against a fixed
anchor, just sorting whichever of the two args is smaller and drawing uniformly between them (or, for
`resolveRandomQuantizedStretchPercent`, restricting to legal ratios within that range, with a NEW fallback
- snap the range's own midpoint to the nearest legal ratio - for the case where NO legal ratio falls
inside a narrow chosen range, e.g. [110,120]; the old model never needed this fallback since one bound was
always exactly 100, itself always a legal ratio). 12 new APVTS parameters (a Min/Max pair per control),
kept deliberately SEPARATE from each control's existing single "manual value" parameter - the manual
parameter is completely unchanged, used only when Random is off; Random mode now reads exclusively from
its own dedicated pair. Hold Bars' own pair is range-restricted to [1,16] at the parameter layer itself
(not just runtime-clamped) - Hold Bars=0 is the dedicated pause state (Docs SS17), a random draw must
never be able to reach it by construction.

**Editor**: each of the 6 gets a genuine two-thumb `juce::Slider` (`SliderStyle::TwoValueHorizontal`,
JUCE's native range-slider mode) occupying the EXACT same layout slot as its manual counterpart, shown
instead of it (never alongside) when that parameter's own Random toggle is on. `AudioProcessorValueTreeState`'s
attachment classes are single-value only, so each range slider is hand-wired: `onValueChange` pushes both
thumb positions into their own Min/Max parameters via `setValueNotifyingHost` (normalised through
`convertTo0to1`, matching what the JUCE attachment classes do internally), `onDragStart`/`onDragEnd`
bracket the whole drag with `beginChangeGesture`/`endChangeGesture` so a host's automation/undo records it
as one gesture rather than a value snapping in with none. `timerCallback` pulls the live parameter values
back into the slider's own display every tick UNLESS the user is actively dragging it
(`isMouseButtonDown()`), covering undo/project-load/any-other-external-change without a second sync path.
Each control's own LABEL doubles as the readout - e.g. "Stretch (%) - 75.0% to 150.0%" - reusing the exact
mechanism built for the Stretch-only version in SS31's own addendum, now generalized and driving ALL 6
through one shared `RandomRangeBinding` struct + `setupRandomRangeBinding` helper rather than six
near-duplicate blocks.

Verified visually via Standalone: Stretch, Hold Bars (integer formatting, no decimal), and Transpose
(symmetric range, e.g. "-12 to 12") all render and drag correctly, writing real parameter changes (dragged
Stretch's max thumb live, watched the label go from "75.0% to 150.0%" to "75.0% to 299.0%" and the actual
parameter value follow). Also confirmed the Connection Matrix round-trip (Docs SS31) doesn't disturb
mid-edit range-slider state - opening and closing the matrix while 3 different Random ranges were active
left all three exactly as set.

6 replaced test blocks + 3 new ones in `OrchDelayLogicCheck.cpp` (still 100% pass): each resolver's own
determinism/bounds checks updated for the new 2-argument signature, PLUS new coverage for the actual point
of this feature - a range that never touches the old anchor at all (e.g. Stretch [110,130] never draws
100% or below), min==max resolving to that exact value (replacing "bound of exactly the anchor" from the
old model), and the Quantized-Stretch no-legal-ratio-in-range fallback. Rebuilt+reinstalled, Build
~2026-09-17. **Not yet live-tested** in Bitwig - Standalone verification covers UI/parameter-wiring
correctness, not real captured-phrase musical behavior.
