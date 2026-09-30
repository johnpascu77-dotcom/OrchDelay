#pragma once

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <vector>
#include <JuceHeader.h>

class OrchDelayAudioProcessor;

// Cross-instance phrase broadcast link (see Docs SS27). Every OrchDelay
// instance owns one. Lets one instance's captured phrases feed another
// instance's Remote memory bank directly - IN PROCESS, no MIDI cable needed
// between tracks - via a loopback TCP hub, following the exact same Hub/
// Client + lock-file-discovery pattern OrchMerge/OrchCapture already use in
// this ecosystem (proven at ~50-100-instance scale). ALL socket work happens
// on this object's own background thread, never the audio or message thread
// (see OrchCaptureLink's own doc comment for why: a blocking connect on the
// shared message thread once froze Bitwig outright, in an earlier plugin in
// this same family).
//
// Every instance is symmetric: it can PUBLISH (if Broadcast Channel > 0)
// and/or SUBSCRIBE (if Listen Channel > 0) at the same time, regardless of
// whether it happens to be the elected hub. Exactly one instance in the
// whole rig should have "Broadcast Hub" on (same manual-designation
// convention as OrchCapture's Coordinator / OrchMerge's Hub toggle, not
// automatic election) - it binds the port and writes the lock file;
// everyone else, hub included, ALSO runs as a client of that hub for its own
// publish/subscribe traffic. The hub's only extra job is a dumb fan-out
// relay: any phrase it receives from one client gets forwarded to every
// OTHER connected client, verbatim - it never filters by channel itself
// (each client decides locally whether an incoming phrase's channel matches
// its own Listen Channel), except that it also applies an incoming phrase to
// its OWN Remote bank when the hub instance's own Listen Channel matches
// (the hub is just another peer, not exempted from listening).
//
// Also carries a second, much smaller kind of traffic (Docs SS33): a
// periodic per-client "heartbeat" (identity + current channel settings, no
// musical content) that only the hub consumes, feeding the Connection
// Matrix UI (Docs SS31), and - one level further - a one-shot control
// command the hub can send back to a specific client to change ITS OWN
// Listen Channel ("click-to-wire"). Both are tagged distinctly from phrase
// messages (a "t" field) so they can never be parsed as one another.
class OrchDelayLink : private juce::Thread
{
public:
    explicit OrchDelayLink (OrchDelayAudioProcessor&);
    ~OrchDelayLink() override;

    // SS38 (2026-09-27): the background thread used to start unconditionally
    // in the constructor, which also runs for any short-lived instance a
    // VST3 host creates purely to validate/scan the plugin (a real,
    // documented category of host behaviour, not specific to this plugin) -
    // that scan instance got a real, permanent socket connection to whatever
    // Hub happened to be running, heartbeating forever with whatever state
    // it was constructed with (frozen at that moment, e.g. a pre-rename
    // Instance Label), showing up in the Connection Matrix as an
    // unkillable "ghost" alongside the real device even though it was never
    // a stale/disconnected entry - it was a second, genuinely live process.
    // Real hosts always call prepareToPlay before actually using an
    // instance; scan/validation instances typically never do. Call this
    // from prepareToPlay instead of starting the thread in the constructor,
    // so a scan instance that's discarded before ever being played never
    // opens a socket in the first place. Idempotent - safe to call on every
    // prepareToPlay (hosts can call it more than once per instance, e.g. on
    // a sample-rate change).
    void start();

    static constexpr int kPort = 47829;

    enum class Mode { Client, Hub, HubPortBusy };
    Mode getModeForUi() const { return mode.load(); }
    bool isConnectedForUi() const { return clientConnected.load(); }

    // Connection Matrix (Docs SS31): one entry per OTHER instance currently
    // known to the hub, built from that instance's own periodic heartbeat
    // (see serviceHeartbeat's own doc comment below) - NOT from the phrase-
    // broadcast traffic, which never carries identity. Only meaningful when
    // THIS instance is itself the hub (empty otherwise, since only the hub
    // ever receives client connections at all) - the editor only shows the
    // matrix when Broadcast Hub is checked, matching that constraint.
    struct RemoteInstanceStatus
    {
        juce::String label;
        int broadcastChannel = 0;
        int listenChannel = 0;
        // SS39 (Hub-pushed presets): carried in the heartbeat alongside
        // broadcast/listen so a captured preset reflects a client's REAL
        // current routing, not just the two fields the Matrix itself needed
        // before this. activeBank is the raw AudioParameterChoice index
        // (0=A,1=B,2=C,3=Remote), matching getActiveBankForUi()'s own range.
        bool relayEnabled = false;
        int activeBank = 0;
        // Added 2026-09-28 alongside PresetEntry's own field - see that
        // struct's doc comment for the live-rig bug this fixes.
        int outputChannelOverride = 0;
        juce::int64 lastSeenMs = 0;

        // Opaque handle for sendSetListenChannel below (Docs SS33/click-to-
        // wire) - the owning HubConnection's own pointer value, reinterpreted
        // as an integer so the editor never touches a raw connection object
        // directly. Never dereferenced as a pointer outside OrchDelayLink
        // itself; only ever compared for identity against the live
        // serverConnections list at send time, so a stale id from a
        // since-disconnected instance safely finds nothing rather than
        // touching freed memory.
        juce::int64 connectionId = 0;
    };
    std::vector<RemoteInstanceStatus> getRemoteStatusesForUi() const;

    // Click-to-wire (Docs SS33): the hub sends a specific OTHER instance a
    // one-shot command to change its own Listen Channel - the first time
    // this codebase lets one instance reach into another's own settings,
    // rather than only ever moving musical content. Only makes sense when
    // THIS instance is the hub (only the hub has any live connectionId to
    // target); a no-op if connectionId no longer matches any connected
    // client (it disconnected between the matrix snapshot and the click -
    // safe, not an error, the matrix will simply stop showing that row next
    // tick). Callable from the message thread (the editor's own click
    // handler) - locks connectionsMutex itself, same as every other
    // accessor here.
    void sendSetListenChannel (juce::int64 connectionId, int channel);

    // SS39: Hub-pushed presets. A preset is just a routing snapshot - one
    // entry per instance, keyed by Instance Label (the only identity every
    // instance already carries and displays, so a saved preset stays
    // meaningful across sessions/reconnects, unlike connectionId which is a
    // live-socket-only handle). Entries are matched by label on the
    // RECEIVING side (each client checks its own getInstanceLabelForUi()
    // against every entry), not resolved to a connectionId on the way out -
    // a preset can legitimately name an instance that isn't connected right
    // now (saved earlier, or the rig's still loading) and it simply won't
    // match anything yet.
    // outputChannelOverride added 2026-09-28 (real live-rig bug): SS42 made
    // every fired note go out on this instance's own resolved real MIDI
    // channel instead of blindly trusting note.channel - correct, but that
    // resolution auto-detects from real incoming note-ons (see OrchDelay
    // Processor.h's own lastSeenChannel doc comment), and a 100%-relay-fed
    // instance with NO live input of its own never sees one, so it stayed
    // stuck on the hardcoded default (1) forever - every relay-fed
    // instrument in a rig ended up dumping its entire output onto whatever
    // real instrument sits on channel 1, exactly the "accumulation on one
    // instrument" symptom this field exists to fix. 0 keeps auto-detect (for
    // an instance that genuinely has live input); any other value pins it
    // explicitly, same convention as the parameter itself.
    struct PresetEntry
    {
        juce::String label;
        int broadcastChannel = 0;
        int listenChannel = 0;
        bool relayEnabled = false;
        int activeBank = 0;
        int outputChannelOverride = 0;
    };

    // Hub-only: one entry per currently-connected client (from the same live
    // heartbeat table getRemoteStatusesForUi() itself reads) PLUS this
    // instance's own current routing - the Hub is a routing participant too
    // (Docs SS31/getRemoteStatusesForUi's own doc comment) and would
    // otherwise be silently missing from its own captured preset. Callable
    // from the editor (message thread); locks connectionsMutex itself.
    std::vector<PresetEntry> capturePresetForUi() const;

    // Hub-only: fans the whole preset out to every connected client - each
    // one filters to its own matching entry itself (see PresetEntry's own
    // doc comment), so sending is a single unicast-free broadcast rather
    // than one send per target. Also applies this instance's OWN matching
    // entry (if present) directly via the processor, since the Hub never
    // hears its own fan-out echoed back to itself (same asymmetry
    // serviceOwnPublish's Hub branch already has). Callable from the editor
    // (message thread); locks connectionsMutex itself.
    void sendPreset (const std::vector<PresetEntry>& entries);

    // SS41: Hub-pushed full reset. A routing preset (SS39 above) reconfigures
    // where content flows; this instead wipes the actual musical content
    // sitting in every instance's phrase memory (all of A/B/C AND Remote) in
    // one click from wherever the user happens to be looking - the rig-wide
    // equivalent of pressing Clear Bank (x3, one per bank) then Clear Remote
    // on every single connected instance by hand, which is exactly the
    // tedious click-count this exists to remove. Fans out to every connected
    // client (unconditionally - unlike a preset, a reset applies to
    // everyone, never filtered by label) and also applies to this instance's
    // own banks directly, since the Hub never hears its own fan-out echoed
    // back to itself (same asymmetry sendPreset above already has). Callable
    // from the editor (message thread); locks connectionsMutex itself.
    void sendResetAll();

    // SS45 (2026-09-28): Hub "General Parameters" - a shared CONTENT-SHAPING
    // template (Hold Bars, Autonomous Fire, Phrase Gap, Minimum Interest,
    // Callback Probability, Monophonic Capture, Ignore Keyswitches on/off),
    // pushed from the Hub's own authored sliders rather than captured from
    // any instance's live state - unlike PresetEntry above (a per-instance
    // routing snapshot, matched by label), this is the SAME template for
    // every connected instance, so it carries no label at all and needs no
    // per-instance matching on receipt - closer in spirit to sendResetAll's
    // own unconditional broadcast than to sendPreset's label-matched one.
    //
    // Deliberately does NOT include KS Ignore Min/Max (real live-rig bug,
    // 2026-09-28): those aren't a rig-wide policy at all, they're a per-
    // instrument safety ceiling (each low instrument's own real lowest
    // playable note) - a uniform push silently clobbered hand-tuned values
    // (Bassoon/Bass Clarinet/etc had already been set below the 0-35
    // default) back to the template's own number. Ignore Keyswitches itself
    // (the on/off toggle) stays here - THAT genuinely is a uniform rig-wide
    // policy, every instrument should have it on. If per-instrument KS
    // ranges ever need a rig-wide tool, it needs its own mechanism that
    // varies by instrument (like OrchNoteMapper's own Dest Max did), not a
    // single shared number pushed to everyone.
    struct ShapeEntry
    {
        int holdBars = 4;
        int autonomousFireBars = 0;
        float phraseGapBeats = 1.0f;
        float minimumInterest = 0.0f;
        float callbackProbability = 0.0f;
        bool monophonicCapture = false;
        bool ignoreKeyswitches = true;
    };

    // Hub-only: pushes `base` to every connected client. When `randomize` is
    // false, every instance (including this one) gets the EXACT same
    // ShapeEntry - a uniform rig-wide policy. When true, each instance
    // (including this one) instead gets its OWN independently jittered copy
    // - see sendShapeToRig's own doc comment in the .cpp for exactly which
    // fields move and by how much (only the 5 numeric fields; the 2 bool
    // policy switches always stay uniform, jittering a bool makes no sense).
    // Unlike sendPreset's single shared wire message, this sends ONE message
    // PER connection (a fresh jittered draw for each, when randomizing), so
    // it can't reuse that broadcast-once shape. Callable from the editor
    // (message thread); locks connectionsMutex itself.
    void sendShapeToRig (const ShapeEntry& base, bool randomize);

private:
    class ClientConnection;
    class HubConnection;
    class HubServer;

    void run() override;
    void reconcileMode();
    void serviceOwnPublish();
    void serviceRelayPublish();
    void serviceHeartbeat();
    void teardownServer();
    static juce::File lockFile();

    void registerHubConnection (std::unique_ptr<HubConnection>);
    void onHubClientMessage (HubConnection*, const juce::var&);
    void onHubClientGone (HubConnection*);

    friend class ClientConnection;
    friend class HubConnection;
    friend class HubServer;

    OrchDelayAudioProcessor& processor;

    std::atomic<Mode> mode { Mode::Client };
    std::atomic<bool> clientConnected { false };

    // owned + touched only by this object's own worker thread (run())
    std::unique_ptr<HubServer> server;
    std::unique_ptr<ClientConnection> client;
    bool wroteLockFile = false;
    int lastPublishedGeneration = -1;
    int lastPublishedRelayGeneration = -1;   // serviceRelayPublish's own tracker, Docs SS35

    mutable std::mutex connectionsMutex;
    std::vector<std::unique_ptr<HubConnection>> serverConnections;

    // Guarded by connectionsMutex too (always touched alongside
    // serverConnections - registered/erased at the same points) rather than
    // its own mutex, to avoid any lock-ordering question between the two.
    // Mutable for the same reason connectionsMutex is: getRemoteStatusesForUi()
    // is logically read-only from the outside but internally self-prunes
    // stale entries (see that function's own comment) - onHubClientGone's
    // disconnect-triggered erase is not the only cleanup path.
    mutable std::map<HubConnection*, RemoteInstanceStatus> remoteStatuses;

    JUCE_DECLARE_WEAK_REFERENCEABLE (OrchDelayLink)
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (OrchDelayLink)
};
