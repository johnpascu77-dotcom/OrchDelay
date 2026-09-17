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
    std::map<HubConnection*, RemoteInstanceStatus> remoteStatuses;

    JUCE_DECLARE_WEAK_REFERENCEABLE (OrchDelayLink)
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (OrchDelayLink)
};
