#include "OrchDelayLink.h"
#include "OrchDelayProcessor.h"
#include "OrchDelayLogic.h"

#include <algorithm>

namespace
{
    constexpr int kPollMs = 300;             // worker loop interval - see OrchDelayLink.h's own doc comment
    constexpr int kConnectTimeoutMs = 300;   // blocking connect - worker thread only, never the message thread

    // Heartbeats arrive every kPollMs (300ms) from a live client. 10x that
    // comfortably tolerates thread-scheduling jitter or an occasional missed
    // tick while still cleaning up promptly if a client's connection died
    // without a clean disconnect signal - onHubClientGone's erase-on-
    // connectionLost is not the only cleanup path (see remoteStatuses' own
    // doc comment in the header for why a second path is needed at all).
    constexpr juce::int64 kHeartbeatStaleMs = 3000;

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
    // so it stays tiny even sent every poll interval. Carries relay/bank too
    // (SS39) so a Hub-captured preset reflects a client's real full routing,
    // not just the two fields the Matrix itself ever needed before this.
    juce::var heartbeatMessageFrom (const juce::String& label, int broadcastChannel, int listenChannel,
                                     bool relayEnabled, int activeBank, int outputChannelOverride)
    {
        auto* obj = new juce::DynamicObject();
        obj->setProperty ("t", "heartbeat");
        obj->setProperty ("label", label);
        obj->setProperty ("bc", broadcastChannel);
        obj->setProperty ("lc", listenChannel);
        obj->setProperty ("relay", relayEnabled);
        obj->setProperty ("bank", activeBank);
        obj->setProperty ("outCh", outputChannelOverride);
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

    // Hub-pushed preset (SS39) - fanned out to every connected client, each
    // one filtering to its own matching entry by label (see
    // OrchDelayLink::PresetEntry's own doc comment).
    juce::var presetMessageFrom (const std::vector<OrchDelayLink::PresetEntry>& entries)
    {
        auto* root = new juce::DynamicObject();
        root->setProperty ("t", "preset");

        juce::Array<juce::var> entriesVar;
        for (const auto& entry : entries)
        {
            auto* obj = new juce::DynamicObject();
            obj->setProperty ("label", entry.label);
            obj->setProperty ("bc", entry.broadcastChannel);
            obj->setProperty ("lc", entry.listenChannel);
            obj->setProperty ("relay", entry.relayEnabled);
            obj->setProperty ("bank", entry.activeBank);
            obj->setProperty ("outCh", entry.outputChannelOverride);
            entriesVar.add (juce::var (obj));
        }
        root->setProperty ("entries", entriesVar);
        return juce::var (root);
    }

    // Hub-pushed full reset (SS41) - deliberately carries no payload at all
    // (unlike presetMessageFrom above, there's nothing to filter by label:
    // every client wipes its own banks unconditionally on receipt).
    juce::var resetMessageFrom()
    {
        auto* obj = new juce::DynamicObject();
        obj->setProperty ("t", "reset");
        return juce::var (obj);
    }

    // Hub "General Parameters" push (SS45) - see OrchDelayLink::ShapeEntry's
    // own doc comment. Flat, no label/entries array - unconditional like
    // resetMessageFrom above, just carrying a real payload this time.
    juce::var shapeMessageFrom (const OrchDelayLink::ShapeEntry& shape)
    {
        auto* obj = new juce::DynamicObject();
        obj->setProperty ("t", "shape");
        obj->setProperty ("holdBars", shape.holdBars);
        obj->setProperty ("autonomousFireBars", shape.autonomousFireBars);
        obj->setProperty ("phraseGapBeats", shape.phraseGapBeats);
        obj->setProperty ("minimumInterest", shape.minimumInterest);
        obj->setProperty ("callbackProbability", shape.callbackProbability);
        obj->setProperty ("monophonicCapture", shape.monophonicCapture);
        obj->setProperty ("ignoreKeyswitches", shape.ignoreKeyswitches);
        return juce::var (obj);
    }

    OrchDelayLink::ShapeEntry shapeEntryFromVar (const juce::var& v)
    {
        OrchDelayLink::ShapeEntry shape;
        shape.holdBars = static_cast<int> (v.getProperty ("holdBars", shape.holdBars));
        shape.autonomousFireBars = static_cast<int> (v.getProperty ("autonomousFireBars", shape.autonomousFireBars));
        shape.phraseGapBeats = static_cast<float> (static_cast<double> (v.getProperty ("phraseGapBeats", shape.phraseGapBeats)));
        shape.minimumInterest = static_cast<float> (static_cast<double> (v.getProperty ("minimumInterest", shape.minimumInterest)));
        shape.callbackProbability = static_cast<float> (static_cast<double> (v.getProperty ("callbackProbability", shape.callbackProbability)));
        shape.monophonicCapture = static_cast<bool> (v.getProperty ("monophonicCapture", shape.monophonicCapture));
        shape.ignoreKeyswitches = static_cast<bool> (v.getProperty ("ignoreKeyswitches", shape.ignoreKeyswitches));
        return shape;
    }

    // Randomize & Send (SS45) - only the 5 numeric fields move, and only by a
    // musically-sensible span per field; the 2 bool policy switches
    // (Monophonic Capture, Ignore Keyswitches) always pass through uniform -
    // see sendShapeToRig's own doc comment for why.
    OrchDelayLink::ShapeEntry jitterShapeEntry (const OrchDelayLink::ShapeEntry& base)
    {
        auto& rng = juce::Random::getSystemRandom();
        OrchDelayLink::ShapeEntry out = base;

        out.holdBars = juce::jlimit (0, 16, base.holdBars + rng.nextInt (juce::Range<int> (-2, 3)));
        out.autonomousFireBars = juce::jlimit (0, 16, base.autonomousFireBars + rng.nextInt (juce::Range<int> (-1, 2)));
        out.phraseGapBeats = juce::jlimit (0.25f, 8.0f,
            base.phraseGapBeats + (rng.nextFloat() * 2.0f - 1.0f) * 0.5f);
        out.minimumInterest = juce::jlimit (0.0f, 100.0f,
            base.minimumInterest + (rng.nextFloat() * 2.0f - 1.0f) * 10.0f);
        out.callbackProbability = juce::jlimit (0.0f, 100.0f,
            base.callbackProbability + (rng.nextFloat() * 2.0f - 1.0f) * 15.0f);

        return out;
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

        if (type == "preset")
        {
            // Hub-pushed preset (SS39): find the ONE entry (if any) whose
            // label matches this instance's own - a preset names every
            // instance in the rig, but each client only ever acts on its own
            // row. Case-insensitive: labels are free-text the user typed,
            // not a normalised identifier, so "Violin 1" and "VIOLIN 1"
            // should both match the same instance rather than silently
            // missing it over a casing difference.
            const auto ownLabel = owner.processor.getInstanceLabelForUi();
            if (auto* entries = parsed.getProperty ("entries", juce::var()).getArray())
            {
                for (const auto& entryVar : *entries)
                {
                    if (! entryVar.getProperty ("label", juce::var()).toString().equalsIgnoreCase (ownLabel))
                        continue;

                    owner.processor.applyPresetFromRemote (
                        static_cast<int> (entryVar.getProperty ("bc", 0)),
                        static_cast<int> (entryVar.getProperty ("lc", 0)),
                        static_cast<bool> (entryVar.getProperty ("relay", false)),
                        static_cast<int> (entryVar.getProperty ("bank", 0)),
                        static_cast<int> (entryVar.getProperty ("outCh", 0)));
                    break;
                }
            }
            return;
        }

        if (type == "reset")
        {
            // Hub-pushed full reset (SS41): unconditional, no label match
            // needed - every client wipes its own banks on receipt.
            owner.processor.resetAllBanksFromRemote();
            return;
        }

        if (type == "shape")
        {
            // Hub "General Parameters" push (SS45): unconditional, same as
            // "reset" above - every client applies whatever this specific
            // message carries (the Hub already resolved uniform vs jittered
            // per-target before sending, see sendShapeToRig). Unpacked into
            // scalars at the call boundary, same as applyPresetFromRemote's
            // own 4-scalar signature - OrchDelayProcessor.h only forward-
            // declares OrchDelayLink, so it can't reference a nested type of
            // it (ShapeEntry) directly.
            const auto shape = shapeEntryFromVar (parsed);
            owner.processor.applyShapeFromRemote (
                shape.holdBars, shape.autonomousFireBars, shape.phraseGapBeats,
                shape.minimumInterest, shape.callbackProbability,
                shape.monophonicCapture, shape.ignoreKeyswitches);
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
}

void OrchDelayLink::start()
{
    if (! isThreadRunning())
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
                                               processor.getListenChannelForUi(),
                                               processor.isRelayEnabledForUi(),
                                               processor.getActiveBankForUi(),
                                               processor.getOutputChannelOverrideForUi());
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
        status.relayEnabled = static_cast<bool> (message.getProperty ("relay", false));
        status.activeBank = static_cast<int> (message.getProperty ("bank", 0));
        status.outputChannelOverride = static_cast<int> (message.getProperty ("outCh", 0));
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

    // Self-prune stale entries before building the result - a real live-rig
    // bug (2026-09-25/26): onHubClientGone's erase only fires on a clean TCP
    // disconnect signal. A client instance torn down/reloaded abruptly by the
    // host (plugin rescan, project reopen, Bitwig's own device-panel
    // suspend/resume cycling) can vanish without ever signalling that,
    // leaving its old entry in remoteStatuses forever - duplicate/stale rows
    // in the Connection Matrix (a reconnected instance shows up as a SECOND
    // row, the original left showing whatever label/channels it had at the
    // moment it went silent) that accumulate the longer a session runs.
    const juce::int64 nowMs = juce::Time::currentTimeMillis();

    for (auto it = remoteStatuses.begin(); it != remoteStatuses.end(); )
    {
        if (nowMs - it->second.lastSeenMs > kHeartbeatStaleMs)
            it = remoteStatuses.erase (it);
        else
            ++it;
    }

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

std::vector<OrchDelayLink::PresetEntry> OrchDelayLink::capturePresetForUi() const
{
    std::vector<PresetEntry> entries;

    // This instance's own current routing first - it's a routing
    // participant too (see this method's own doc comment in the header) and
    // would otherwise be silently missing from its own captured preset.
    {
        PresetEntry own;
        own.label = processor.getInstanceLabelForUi();
        own.broadcastChannel = processor.getBroadcastChannelForUi();
        own.listenChannel = processor.getListenChannelForUi();
        own.relayEnabled = processor.isRelayEnabledForUi();
        own.activeBank = processor.getActiveBankForUi();
        own.outputChannelOverride = processor.getOutputChannelOverrideForUi();
        entries.push_back (own);
    }

    for (const auto& status : getRemoteStatusesForUi())
    {
        PresetEntry entry;
        entry.label = status.label;
        entry.broadcastChannel = status.broadcastChannel;
        entry.listenChannel = status.listenChannel;
        entry.relayEnabled = status.relayEnabled;
        entry.activeBank = status.activeBank;
        entry.outputChannelOverride = status.outputChannelOverride;
        entries.push_back (entry);
    }

    return entries;
}

void OrchDelayLink::sendPreset (const std::vector<PresetEntry>& entries)
{
    const auto message = presetMessageFrom (entries);

    {
        std::lock_guard<std::mutex> lock (connectionsMutex);
        for (auto& connection : serverConnections)
            sendJson (*connection, message);
    }

    // The Hub never hears its own fan-out echoed back to itself (same
    // asymmetry serviceOwnPublish's Hub branch already has for phrases) - so
    // apply this instance's own matching entry directly instead, exactly the
    // same way a client would filter for it on the receiving end.
    const auto ownLabel = processor.getInstanceLabelForUi();
    for (const auto& entry : entries)
    {
        if (! entry.label.equalsIgnoreCase (ownLabel))
            continue;

        processor.applyPresetFromRemote (entry.broadcastChannel, entry.listenChannel,
                                          entry.relayEnabled, entry.activeBank,
                                          entry.outputChannelOverride);
        break;
    }
}

void OrchDelayLink::sendResetAll()
{
    const auto message = resetMessageFrom();

    {
        std::lock_guard<std::mutex> lock (connectionsMutex);
        for (auto& connection : serverConnections)
            sendJson (*connection, message);
    }

    // Same asymmetry as sendPreset above - the Hub never hears its own
    // fan-out echoed back to itself, so apply the reset to this instance's
    // own banks directly instead.
    processor.resetAllBanksFromRemote();
}

void OrchDelayLink::sendShapeToRig (const ShapeEntry& base, bool randomize)
{
    // One message PER connection, not one shared broadcast - unlike
    // sendPreset/sendResetAll above, Randomize needs a genuinely different
    // draw for each target (see jitterShapeEntry's own doc comment); the
    // uniform (non-randomize) path just happens to send the same payload
    // every time, which is still correct to do per-connection rather than
    // precomputing one shared juce::var, since it keeps this one code path
    // covering both cases.
    {
        std::lock_guard<std::mutex> lock (connectionsMutex);
        for (auto& connection : serverConnections)
        {
            const auto shape = randomize ? jitterShapeEntry (base) : base;
            sendJson (*connection, shapeMessageFrom (shape));
        }
    }

    // Same asymmetry as sendPreset/sendResetAll above - the Hub never hears
    // its own fan-out echoed back to itself, so apply this instance's own
    // (independently jittered, if randomizing) copy directly instead.
    const auto ownShape = randomize ? jitterShapeEntry (base) : base;
    processor.applyShapeFromRemote (
        ownShape.holdBars, ownShape.autonomousFireBars, ownShape.phraseGapBeats,
        ownShape.minimumInterest, ownShape.callbackProbability,
        ownShape.monophonicCapture, ownShape.ignoreKeyswitches);
}
