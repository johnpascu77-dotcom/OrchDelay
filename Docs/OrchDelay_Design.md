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
reinstalled, Build ~17:23 UTC. Live-retest pending.
