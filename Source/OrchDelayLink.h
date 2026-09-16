#pragma once

#include <atomic>
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
class OrchDelayLink : private juce::Thread
{
public:
    explicit OrchDelayLink (OrchDelayAudioProcessor&);
    ~OrchDelayLink() override;

    static constexpr int kPort = 47829;

    enum class Mode { Client, Hub, HubPortBusy };
    Mode getModeForUi() const { return mode.load(); }
    bool isConnectedForUi() const { return clientConnected.load(); }

private:
    class ClientConnection;
    class HubConnection;
    class HubServer;

    void run() override;
    void reconcileMode();
    void serviceOwnPublish();
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

    std::mutex connectionsMutex;
    std::vector<std::unique_ptr<HubConnection>> serverConnections;

    JUCE_DECLARE_WEAK_REFERENCEABLE (OrchDelayLink)
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (OrchDelayLink)
};
