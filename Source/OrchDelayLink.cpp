#include "OrchDelayLink.h"
#include "OrchDelayProcessor.h"
#include "OrchDelayLogic.h"

#include <algorithm>

namespace
{
    constexpr int kPollMs = 300;             // worker loop interval - see OrchDelayLink.h's own doc comment
    constexpr int kConnectTimeoutMs = 300;   // blocking connect - worker thread only, never the message thread

    void sendJson (juce::InterprocessConnection& connection, const juce::var& value)
    {
        const auto json = juce::JSON::toString (value, true);
        connection.sendMessage (juce::MemoryBlock (json.toRawUTF8(), json.getNumBytesAsUTF8()));
    }

    juce::var phraseMessageFrom (int channel, const odly::MemoryEntry& entry)
    {
        auto entryVar = odly::memoryEntryToVar (entry);
        if (auto* obj = entryVar.getDynamicObject())
        {
            obj->setProperty ("t", "phrase");
            obj->setProperty ("ch", channel);
        }
        return entryVar;
    }

    // Connection Matrix identity ping (Docs SS31) - deliberately a separate
    // message shape from phraseMessageFrom's own, never carrying note data,
    // so it stays tiny even sent every poll interval.
    juce::var heartbeatMessageFrom (const juce::String& label, int broadcastChannel, int listenChannel)
    {
        auto* obj = new juce::DynamicObject();
        obj->setProperty ("t", "heartbeat");
        obj->setProperty ("label", label);
        obj->setProperty ("bc", broadcastChannel);
        obj->setProperty ("lc", listenChannel);
        return juce::var (obj);
    }

    // Click-to-wire command (Docs SS33) - unicast, hub to one specific
    // client, never fanned out. Tells that instance to set its OWN Listen
    // Channel to `channel` (0 disconnects it).
    juce::var setListenMessageFrom (int channel)
    {
        auto* obj = new juce::DynamicObject();
        obj->setProperty ("t", "setListen");
        obj->setProperty ("ch", channel);
        return juce::var (obj);
    }
}

// ===================== connection / server objects =====================
//
// callbacksOnMessageThread == false: every connection callback arrives on the
// connection's own background thread, never the host message thread.

class OrchDelayLink::ClientConnection : public juce::InterprocessConnection
{
public:
    explicit ClientConnection (OrchDelayLink& o) : juce::InterprocessConnection (false), owner (o) {}
    ~ClientConnection() override { disconnect(); }

    void connectionMade() override { owner.clientConnected.store (true); }
    void connectionLost() override { owner.clientConnected.store (false); }

    // A message here is normally a "phrase" relayed by the hub, originating
    // from ANOTHER client - the hub never relays a client's own message back
    // to that same client (see OrchDelayLink.h's own doc comment), so there
    // is no self-hear case to guard against here. Heartbeats (Docs SS31) are
    // never fanned out to other clients (only the hub itself consumes them,
    // to build the Connection Matrix), but check the type explicitly anyway
    // rather than assume - cheap, and correct if that ever changes.
    void messageReceived (const juce::MemoryBlock& message) override
    {
        const auto json = juce::String::fromUTF8 (static_cast<const char*> (message.getData()),
                                                  static_cast<int> (message.getSize()));
        juce::var parsed;
        if (! juce::JSON::parse (json, parsed).wasOk() || ! parsed.isObject())
            return;

        const auto type = parsed.getProperty ("t", juce::var()).toString();

        if (type == "setListen")
        {
            // Click-to-wire (Docs SS33): the hub telling THIS instance to
            // change its own Listen Channel. Applied on the message thread,
            // never here on the connection's own background thread - see
            // OrchDelayAudioProcessor::setListenChannelFromRemote's own doc
            // comment.
            owner.processor.setListenChannelFromRemote (static_cast<int> (parsed.getProperty ("ch", 0)));
            return;
        }

        if (type != "phrase")
            return;

        const int listenChannel = owner.processor.getListenChannelForUi();
        if (listenChannel <= 0)
            return;

        const int messageChannel = static_cast<int> (parsed.getProperty ("ch", 0));
        if (messageChannel != listenChannel)
            return;

        owner.processor.pushIncomingRemotePhrase (odly::memoryEntryFromVar (parsed));
    }

private:
    OrchDelayLink& owner;
};

class OrchDelayLink::HubConnection : public juce::InterprocessConnection
{
public:
    explicit HubConnection (OrchDelayLink& ownerIn)
        : juce::InterprocessConnection (false), owner (ownerIn) {}

    ~HubConnection() override { disconnect(); }

    void connectionMade() override {}
    void connectionLost() override { owner.onHubClientGone (this); }

    void messageReceived (const juce::MemoryBlock& message) override
    {
        const auto json = juce::String::fromUTF8 (static_cast<const char*> (message.getData()),
                                                  static_cast<int> (message.getSize()));
        juce::var parsed;
        if (juce::JSON::parse (json, parsed).wasOk() && parsed.isObject())
            owner.onHubClientMessage (this, parsed);
    }

private:
    OrchDelayLink& owner;
};

class OrchDelayLink::HubServer : public juce::InterprocessConnectionServer
{
public:
    explicit HubServer (OrchDelayLink& ownerIn) : owner (ownerIn) {}
    ~HubServer() override { stop(); }

    juce::InterprocessConnection* createConnectionObject() override
    {
        auto connection = std::make_unique<HubConnection> (owner);
        auto* raw = connection.get();
        owner.registerHubConnection (std::move (connection));
        return raw;
    }

private:
    OrchDelayLink& owner;
};

// ============================== link ==================================

OrchDelayLink::OrchDelayLink (OrchDelayAudioProcessor& p)
    : juce::Thread ("OrchDelay link"), processor (p)
{
    startThread (juce::Thread::Priority::background);
}

OrchDelayLink::~OrchDelayLink()
{
    signalThreadShouldExit();
    notify();
    stopThread (3000);

    // run() already tore these down on its way out; harmless to repeat.
    client.reset();
    teardownServer();
}

juce::File OrchDelayLink::lockFile()
{
    return juce::File::getSpecialLocation (juce::File::tempDirectory)
               .getChildFile ("orchdelay-hub.lock");
}

void OrchDelayLink::teardownServer()
{
    if (server != nullptr)
    {
        server->stop();
        server.reset();
    }

    {
        std::lock_guard<std::mutex> lock (connectionsMutex);
        serverConnections.clear();
    }

    if (wroteLockFile)
    {
        lockFile().deleteFile();
        wroteLockFile = false;
    }
}

// ---- worker thread ----

void OrchDelayLink::run()
{
    while (! threadShouldExit())
    {
        reconcileMode();

        if (mode.load() == Mode::Client)
        {
            if (client != nullptr && ! client->isConnected())
            {
                clientConnected.store (false);

                // The gate: only reach for the socket when a hub has
                // announced itself. No hub on the rig == one cheap file
                // check per poll.
                if (lockFile().existsAsFile())
                    client->connectToSocket ("127.0.0.1", kPort, kConnectTimeoutMs);
            }
        }

        serviceOwnPublish();
        serviceRelayPublish();
        serviceHeartbeat();

        wait (kPollMs);
    }

    client.reset();
    teardownServer();
}

void OrchDelayLink::reconcileMode()
{
    const bool wantHub = processor.isLinkHubParamOn();

    if (wantHub)
    {
        if (client != nullptr)
        {
            client.reset();
            clientConnected.store (false);
        }

        if (server == nullptr)
        {
            auto candidate = std::make_unique<HubServer> (*this);
            if (candidate->beginWaitingForSocket (kPort))
            {
                server = std::move (candidate);
                mode.store (Mode::Hub);
            }
            else
            {
                mode.store (Mode::HubPortBusy);
            }
        }
        else
        {
            mode.store (Mode::Hub);
        }

        if (server != nullptr && ! wroteLockFile)
        {
            lockFile().replaceWithText (juce::String (kPort));
            wroteLockFile = true;
        }
    }
    else
    {
        if (server != nullptr || wroteLockFile)
            teardownServer();

        mode.store (Mode::Client);

        if (client == nullptr)
        {
            client = std::make_unique<ClientConnection> (*this);
            lastPublishedGeneration = -1;
            lastPublishedRelayGeneration = -1;
        }
    }
}

// Shared by both roles: notice "there's a new LOCAL phrase to publish" the
// same way OrchCaptureLink notices "the take generation changed", then send
// it out - to the hub (Client role) or fanned out to every connected client
// (Hub role). Never applied to this SAME instance's own Remote bank in
// either role - Remote is exclusively for OTHER instances' material (see
// OrchDelayLink.h's own doc comment).
void OrchDelayLink::serviceOwnPublish()
{
    const int channel = processor.getBroadcastChannelForUi();
    if (channel <= 0)
        return;

    const int generation = processor.getCapturedGenerationForUi();
    if (generation == lastPublishedGeneration)
        return;
    lastPublishedGeneration = generation;

    const auto entry = processor.snapshotLastCapturedForBroadcast();
    const auto message = phraseMessageFrom (channel, entry);

    if (mode.load() == Mode::Hub)
    {
        std::lock_guard<std::mutex> lock (connectionsMutex);
        for (auto& connection : serverConnections)
            sendJson (*connection, message);
    }
    else if (client != nullptr && client->isConnected())
    {
        sendJson (*client, message);
    }
}

// Relay (Docs SS35) - a second, independent "is there something new to
// publish" check, tracked separately from serviceOwnPublish's own
// generation counter (see lastRelayEntry's own doc comment in the header
// for why: a live capture and a relay firing can happen in the same block,
// sharing one channel would let one silently clobber the other). Otherwise
// an exact structural mirror of serviceOwnPublish above - same channel to
// send on (this instance's own Broadcast Channel; relaying uses the SAME
// channel as first-hand captures, not a separate one), same Hub-fan-out-
// vs-Client-single-send split. The hop count itself already rode along
// inside the entry (see odly::MemoryEntry::hopCount) - nothing extra to do
// with it here, phraseMessageFrom/memoryEntryToVar already serialize it.
void OrchDelayLink::serviceRelayPublish()
{
    const int channel = processor.getBroadcastChannelForUi();
    if (channel <= 0)
        return;

    const int generation = processor.getRelayGenerationForUi();
    if (generation == lastPublishedRelayGeneration)
        return;
    lastPublishedRelayGeneration = generation;

    const auto entry = processor.snapshotLastRelayForBroadcast();
    const auto message = phraseMessageFrom (channel, entry);

    if (mode.load() == Mode::Hub)
    {
        std::lock_guard<std::mutex> lock (connectionsMutex);
        for (auto& connection : serverConnections)
            sendJson (*connection, message);
    }
    else if (client != nullptr && client->isConnected())
    {
        sendJson (*client, message);
    }
}

// Connection Matrix identity ping (Docs SS31) - client role only. The hub
// never needs to send ITSELF a heartbeat over the network: its own editor
// reads its own label/channels directly (see getRemoteStatusesForUi's own
// doc comment). Sent every poll regardless of channel settings, including
// 0/0 (off) - the matrix is meant to show every instance in the rig, wired
// or not, so a forgotten-to-configure instance is visible rather than
// silently absent.
void OrchDelayLink::serviceHeartbeat()
{
    if (mode.load() != Mode::Client || client == nullptr || ! client->isConnected())
        return;

    const auto message = heartbeatMessageFrom (processor.getInstanceLabelForUi(),
                                               processor.getBroadcastChannelForUi(),
                                               processor.getListenChannelForUi());
    sendJson (*client, message);
}

// ---- hub side (connection threads) ----

void OrchDelayLink::registerHubConnection (std::unique_ptr<HubConnection> connection)
{
    std::lock_guard<std::mutex> lock (connectionsMutex);
    serverConnections.push_back (std::move (connection));
}

void OrchDelayLink::onHubClientMessage (HubConnection* sender, const juce::var& message)
{
    const auto type = message.getProperty ("t", juce::var()).toString();

    if (type == "heartbeat")
    {
        // Consumed here only - never fanned out (no other client has any use
        // for it, only the hub's own Connection Matrix does) and never
        // treated as phrase content.
        RemoteInstanceStatus status;
        status.label = message.getProperty ("label", "").toString();
        status.broadcastChannel = static_cast<int> (message.getProperty ("bc", 0));
        status.listenChannel = static_cast<int> (message.getProperty ("lc", 0));
        status.lastSeenMs = juce::Time::currentTimeMillis();
        status.connectionId = reinterpret_cast<juce::int64> (sender);

        std::lock_guard<std::mutex> lock (connectionsMutex);
        remoteStatuses[sender] = status;
        return;
    }

    if (type != "phrase")
        return;

    // Dumb fan-out relay to every OTHER connected client - the hub never
    // filters by channel itself, each client decides locally whether it
    // cares (see OrchDelayLink.h's own doc comment).
    {
        std::lock_guard<std::mutex> lock (connectionsMutex);
        for (auto& connection : serverConnections)
            if (connection.get() != sender)
                sendJson (*connection, message);
    }

    // The hub is also a peer, not exempt from listening: if ITS OWN Listen
    // Channel matches, fold this (necessarily OTHER-instance-originated -
    // it arrived over a client connection) phrase into its own Remote bank
    // too.
    const int listenChannel = processor.getListenChannelForUi();
    if (listenChannel <= 0)
        return;

    const int messageChannel = static_cast<int> (message.getProperty ("ch", 0));
    if (messageChannel != listenChannel)
        return;

    processor.pushIncomingRemotePhrase (odly::memoryEntryFromVar (message));
}

void OrchDelayLink::onHubClientGone (HubConnection* connection)
{
    juce::WeakReference<OrchDelayLink> weak (this);
    juce::MessageManager::callAsync ([weak, connection]
    {
        if (auto* self = weak.get())
        {
            std::lock_guard<std::mutex> lock (self->connectionsMutex);
            self->serverConnections.erase (
                std::remove_if (self->serverConnections.begin(), self->serverConnections.end(),
                                [connection] (const std::unique_ptr<HubConnection>& entry)
                                { return entry.get() == connection; }),
                self->serverConnections.end());
            self->remoteStatuses.erase (connection);
        }
    });
}

std::vector<OrchDelayLink::RemoteInstanceStatus> OrchDelayLink::getRemoteStatusesForUi() const
{
    // Called only from the editor (message thread) - blocking lock is fine,
    // same reasoning as every other non-audio-thread accessor in this file.
    std::lock_guard<std::mutex> lock (connectionsMutex);
    std::vector<RemoteInstanceStatus> result;
    result.reserve (remoteStatuses.size());
    for (const auto& [connection, status] : remoteStatuses)
        result.push_back (status);
    return result;
}

void OrchDelayLink::sendSetListenChannel (juce::int64 connectionId, int channel)
{
    // Called from the editor's own click handler (message thread) - locks
    // connectionsMutex itself like every other accessor, same as the
    // reasoning in this method's own doc comment in the header.
    std::lock_guard<std::mutex> lock (connectionsMutex);

    for (auto& connection : serverConnections)
    {
        if (reinterpret_cast<juce::int64> (connection.get()) == connectionId)
        {
            sendJson (*connection, setListenMessageFrom (channel));
            return;
        }
    }
    // Not found - the target disconnected between the matrix snapshot and
    // the click. Safe no-op, see this method's own doc comment.
}
