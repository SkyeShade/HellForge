// The Party stand-in's networks (party_shim.cpp): how one ends, how its queued operations run, and how it connects, on
// this machine or through the local HellForge backend (link.h). Included by party_shim.cpp alone, after party_state.h.
#pragma once
#include "party_state.h"

namespace {

void RetireIfUnused(Device* device);

// Ends a network: its endpoints and invitations are destroyed first, then the network itself.
void DestroyNetwork(Network* network, PARTY_DESTROYED_REASON reason)
{
    for (Endpoint* endpoint : network->endpoints)
        if (!endpoint->destroyed) EndpointDestroyed(network, endpoint, reason);
    for (Invitation* invitation : network->invitations)
        if (!invitation->destroyed) InvitationDestroyed(network, invitation, reason);
    for (Device* device : network->devices)
    {
        PARTY_REMOTE_DEVICE_LEFT_NETWORK_STATE_CHANGE& left = Enqueue(PARTY_STATE_CHANGE_TYPE_REMOTE_DEVICE_LEFT_NETWORK)->payload.remoteDeviceLeft;
        left.reason = reason;
        left.device = Handle(device);
        left.network = Handle(network);
    }
    std::vector<Device*> leaving;
    leaving.swap(network->devices);
    for (Device* device : leaving) RetireIfUnused(device);
    PARTY_NETWORK_DESTROYED_STATE_CHANGE& destroyed = Enqueue(PARTY_STATE_CHANGE_TYPE_NETWORK_DESTROYED)->payload.networkDestroyed;
    destroyed.reason = reason;
    destroyed.errorDetail = reason == PARTY_DESTROYED_REASON_REQUESTED ? c_partyErrorSuccess : unavailable;
    destroyed.network = Handle(network);
    network->state = NetworkState::Destroyed;
    network->authenticatedUsers.clear();
}

// Completes an operation that cannot run: its network failed to connect or is gone.
void FailOperation(Network* network, Operation& operation, PARTY_STATE_CHANGE_RESULT failure)
{
    switch (operation.type)
    {
    case PARTY_STATE_CHANGE_TYPE_AUTHENTICATE_LOCAL_USER_COMPLETED:
    {
        StateChange* change = Enqueue(operation.type);
        PARTY_AUTHENTICATE_LOCAL_USER_COMPLETED_STATE_CHANGE& completed = change->payload.authenticateLocalUser;
        CopyString(change->invitationIdentifier, sizeof(change->invitationIdentifier), operation.invitationIdentifier.c_str());
        completed.result = failure;
        completed.errorDetail = unavailable;
        completed.network = Handle(network);
        completed.localUser = operation.localUser;
        completed.invitationIdentifier = change->invitationIdentifier;
        completed.asyncIdentifier = operation.asyncIdentifier;
        break;
    }
    case PARTY_STATE_CHANGE_TYPE_CREATE_ENDPOINT_COMPLETED:
    {
        PARTY_CREATE_ENDPOINT_COMPLETED_STATE_CHANGE& completed = Enqueue(operation.type)->payload.createEndpoint;
        completed.result = failure;
        completed.errorDetail = unavailable;
        completed.network = Handle(network);
        completed.localUser = operation.localUser;
        completed.asyncIdentifier = operation.asyncIdentifier;
        completed.localEndpoint = Handle(operation.endpoint);
        if (!operation.endpoint->destroyed) EndpointDestroyed(network, operation.endpoint, PARTY_DESTROYED_REASON_CREATION_FAILED);
        break;
    }
    case PARTY_STATE_CHANGE_TYPE_CREATE_INVITATION_COMPLETED:
    {
        PARTY_CREATE_INVITATION_COMPLETED_STATE_CHANGE& completed = Enqueue(operation.type)->payload.createInvitation;
        completed.result = failure;
        completed.errorDetail = unavailable;
        completed.network = Handle(network);
        completed.localUser = operation.localUser;
        completed.asyncIdentifier = operation.asyncIdentifier;
        completed.invitation = Handle(operation.invitation);
        if (!operation.invitation->destroyed) InvitationDestroyed(network, operation.invitation, PARTY_DESTROYED_REASON_CREATION_FAILED);
        break;
    }
    case PARTY_STATE_CHANGE_TYPE_DESTROY_ENDPOINT_COMPLETED:
    {
        PARTY_DESTROY_ENDPOINT_COMPLETED_STATE_CHANGE& completed = Enqueue(operation.type)->payload.destroyEndpoint;
        completed.result = failure;
        completed.errorDetail = unavailable;
        completed.network = Handle(network);
        completed.localEndpoint = Handle(operation.endpoint);
        completed.asyncIdentifier = operation.asyncIdentifier;
        break;
    }
    case PARTY_STATE_CHANGE_TYPE_LEAVE_NETWORK_COMPLETED:
        LeaveCompleted(network, operation.asyncIdentifier); // Leaving a network that is already gone succeeds.
        break;
    default:
        break;
    }
}

// Users join by naming an invitation of the network; the host uses the initial one. A friend's network was already
// admitted by its host's backend (lobby members only), so its invitations are not checked here.
void AuthenticateUser(Network* network, Operation& operation)
{
    bool invited = network->viaBackend;
    for (Invitation* invitation : network->invitations)
        if (!invitation->destroyed && invitation->spec.identifier == operation.invitationIdentifier) invited = true;
    if (!invited || Find(operation.localUser) == nullptr)
    {
        FailOperation(network, operation, PARTY_STATE_CHANGE_RESULT_USER_NOT_AUTHORIZED);
        return;
    }
    if (!IsAuthenticated(network, operation.localUser)) network->authenticatedUsers.push_back(operation.localUser);
    StateChange* change = Enqueue(operation.type);
    PARTY_AUTHENTICATE_LOCAL_USER_COMPLETED_STATE_CHANGE& completed = change->payload.authenticateLocalUser;
    CopyString(change->invitationIdentifier, sizeof(change->invitationIdentifier), operation.invitationIdentifier.c_str());
    completed.result = PARTY_STATE_CHANGE_RESULT_SUCCEEDED;
    completed.network = Handle(network);
    completed.localUser = operation.localUser;
    completed.invitationIdentifier = change->invitationIdentifier;
    completed.asyncIdentifier = operation.asyncIdentifier;
}

// A local endpoint that has its id, announced to the backend: [network, "", id, its owner's entity id].
void AnnounceEndpoint(Network* network, const Operation& operation)
{
    LocalUser* owner = Find(operation.localUser);
    link.Send(hellforge::Frame("party.endpoint").Add(network->descriptor.networkIdentifier).Add("").Add((long long)operation.endpoint->uniqueIdentifier)
        .Add(owner != nullptr ? owner->entityId : ""));
}

// A local endpoint's creation succeeded: ENDPOINT_CREATED, then its CREATE_ENDPOINT_COMPLETED.
void EndpointCreated(Network* network, const Operation& operation)
{
    PARTY_ENDPOINT_CREATED_STATE_CHANGE& created = Enqueue(PARTY_STATE_CHANGE_TYPE_ENDPOINT_CREATED)->payload.endpointCreated;
    created.network = Handle(network);
    created.endpoint = Handle(operation.endpoint);
    PARTY_CREATE_ENDPOINT_COMPLETED_STATE_CHANGE& completed = Enqueue(PARTY_STATE_CHANGE_TYPE_CREATE_ENDPOINT_COMPLETED)->payload.createEndpoint;
    completed.result = PARTY_STATE_CHANGE_RESULT_SUCCEEDED;
    completed.network = Handle(network);
    completed.localUser = operation.localUser;
    completed.asyncIdentifier = operation.asyncIdentifier;
    completed.localEndpoint = Handle(operation.endpoint);
}

// In a friend's network the host's backend assigns the endpoint id; the creation completes when it answers.
void RequestEndpointId(Network* network, const Operation& operation)
{
    std::string id = "e" + std::to_string(++operationCounter);
    LocalUser* owner = Find(operation.localUser);
    network->waiting[id] = operation;
    link.Send(hellforge::Frame("party.endpoint").Add(network->descriptor.networkIdentifier).Add(id).Add("").Add(owner != nullptr ? owner->entityId : ""));
}

void CreateEndpoint(Network* network, Operation& operation)
{
    // An endpoint owned by a user needs that user authenticated (queued earlier operations ran first).
    if (operation.endpoint->destroyed || (operation.localUser != nullptr && !IsAuthenticated(network, operation.localUser)))
    {
        FailOperation(network, operation, PARTY_STATE_CHANGE_RESULT_USER_NOT_AUTHORIZED);
        return;
    }
    if (network->viaBackend && network->endpointBlock != 0 && network->blockUsed < 64)
    {
        // In a friend's network this machine's endpoints take ids from the block the host gave it, so they exist at
        // once: a joining game sends from its endpoint as soon as it sees the host's.
        operation.endpoint->created = true;
        operation.endpoint->uniqueIdentifier = (uint16_t)(network->endpointBlock + network->blockUsed++);
        AnnounceEndpoint(network, operation);
        EndpointCreated(network, operation);
        return;
    }
    if (network->viaBackend)
    {
        RequestEndpointId(network, operation);
        return;
    }
    operation.endpoint->created = true;
    operation.endpoint->uniqueIdentifier = network->nextEndpointIdentifier++;
    if (remote && network->hosted != nullptr) AnnounceEndpoint(network, operation);
    EndpointCreated(network, operation);
}

void CreateInvitation(Network* network, Operation& operation)
{
    if (!IsAuthenticated(network, operation.localUser))
    {
        FailOperation(network, operation, PARTY_STATE_CHANGE_RESULT_USER_NOT_AUTHORIZED);
        return;
    }
    PARTY_CREATE_INVITATION_COMPLETED_STATE_CHANGE& completed = Enqueue(operation.type)->payload.createInvitation;
    completed.result = PARTY_STATE_CHANGE_RESULT_SUCCEEDED;
    completed.network = Handle(network);
    completed.localUser = operation.localUser;
    completed.asyncIdentifier = operation.asyncIdentifier;
    completed.invitation = Handle(operation.invitation);
}

void DestroyEndpoint(Network* network, Operation& operation)
{
    if (operation.endpoint->destroyed)
    {
        FailOperation(network, operation, PARTY_STATE_CHANGE_RESULT_UNKNOWN_ERROR);
        return;
    }
    if (remote && (network->viaBackend || network->hosted != nullptr) && operation.endpoint->created)
        link.Send(hellforge::Frame("party.endpointGone").Add(network->descriptor.networkIdentifier).Add((long long)operation.endpoint->uniqueIdentifier));
    EndpointDestroyed(network, operation.endpoint, PARTY_DESTROYED_REASON_REQUESTED);
    PARTY_DESTROY_ENDPOINT_COMPLETED_STATE_CHANGE& completed = Enqueue(operation.type)->payload.destroyEndpoint;
    completed.result = PARTY_STATE_CHANGE_RESULT_SUCCEEDED;
    completed.network = Handle(network);
    completed.localEndpoint = Handle(operation.endpoint);
    completed.asyncIdentifier = operation.asyncIdentifier;
}

// The SDK reports NETWORK_DESTROYED before the leave completion.
void LeaveNetwork(Network* network, const Operation& operation)
{
    if (remote && (network->viaBackend || network->hosted != nullptr)) link.Send(hellforge::Frame("party.leave").Add(network->descriptor.networkIdentifier));
    DestroyNetwork(network, PARTY_DESTROYED_REASON_REQUESTED);
    LeaveCompleted(network, operation.asyncIdentifier);
}

// Runs an operation on a connected network.
void RunOperation(Network* network, Operation& operation)
{
    if (network->state != NetworkState::Connected)
    {
        FailOperation(network, operation, PARTY_STATE_CHANGE_RESULT_NETWORK_NO_LONGER_EXISTS);
        return;
    }
    switch (operation.type)
    {
    case PARTY_STATE_CHANGE_TYPE_AUTHENTICATE_LOCAL_USER_COMPLETED: AuthenticateUser(network, operation); break;
    case PARTY_STATE_CHANGE_TYPE_CREATE_ENDPOINT_COMPLETED: CreateEndpoint(network, operation); break;
    case PARTY_STATE_CHANGE_TYPE_CREATE_INVITATION_COMPLETED: CreateInvitation(network, operation); break;
    case PARTY_STATE_CHANGE_TYPE_DESTROY_ENDPOINT_COMPLETED: DestroyEndpoint(network, operation); break;
    case PARTY_STATE_CHANGE_TYPE_LEAVE_NETWORK_COMPLETED: LeaveNetwork(network, operation); break;
    default:
        break;
    }
}

// The host a network descriptor names: "hellforge-host:<entity id>" in its connection information.
std::string HostEntity(const PARTY_NETWORK_DESCRIPTOR& descriptor)
{
    const char* text = reinterpret_cast<const char*>(descriptor.opaqueConnectionInformation);
    size_t length = strnlen(text, sizeof(descriptor.opaqueConnectionInformation));
    size_t prefix = sizeof(hostPrefix) - 1;
    return length > prefix && strncmp(text, hostPrefix, prefix) == 0 ? std::string(text + prefix, length - prefix) : std::string();
}

Network* FindNetwork(const std::string& identifier)
{
    for (Network* network : networks)
        if (network->state != NetworkState::Destroyed && strncmp(network->descriptor.networkIdentifier, identifier.c_str(), sizeof(network->descriptor.networkIdentifier)) == 0)
            return network;
    return nullptr;
}

// A friend's machine as a remote device; REMOTE_DEVICE_CREATED when it is seen while no live device stands for it.
Device* DeviceFor(const std::string& node)
{
    for (Device* device : remoteDevices)
        if (device->node == node && !device->destroyed) return device;
    Device* device = new Device{};
    device->node = node;
    remoteDevices.push_back(device);
    Enqueue(PARTY_STATE_CHANGE_TYPE_REMOTE_DEVICE_CREATED)->payload.remoteDeviceCreated.device = Handle(device);
    return device;
}

// A friend's machine joins a network (once).
Device* JoinDevice(Network* network, const std::string& node)
{
    Device* device = DeviceFor(node);
    for (Device* member : network->devices)
        if (member == device) return device;
    network->devices.push_back(device);
    PARTY_REMOTE_DEVICE_JOINED_NETWORK_STATE_CHANGE& joined = Enqueue(PARTY_STATE_CHANGE_TYPE_REMOTE_DEVICE_JOINED_NETWORK)->payload.remoteDeviceJoined;
    joined.device = Handle(device);
    joined.network = Handle(network);
    hellforge::Note("party link: device %s joined network %s", node.c_str(), network->descriptor.networkIdentifier);
    return device;
}

void AddRemoteEndpoint(Network* network, const std::string& node, uint16_t uid, const std::string& entityId)
{
    for (Endpoint* endpoint : network->endpoints)
        if (!endpoint->destroyed && endpoint->created && endpoint->uniqueIdentifier == uid) return;
    Endpoint* endpoint = new Endpoint{};
    endpoint->network = network;
    endpoint->device = JoinDevice(network, node);
    endpoint->entityId = entityId;
    endpoint->uniqueIdentifier = uid;
    endpoint->created = true;
    network->endpoints.push_back(endpoint);
    PARTY_ENDPOINT_CREATED_STATE_CHANGE& created = Enqueue(PARTY_STATE_CHANGE_TYPE_ENDPOINT_CREATED)->payload.endpointCreated;
    created.network = Handle(network);
    created.endpoint = Handle(endpoint);
}

// Like the SDK, a remote device that shares no network with this machine any more is destroyed. HD2 drops a join whose
// host machine joins without REMOTE_DEVICE_CREATED (second two-PC test: the friend who had hosted the other player
// could not join them back).
void RetireIfUnused(Device* device)
{
    if (device->destroyed) return;
    for (Network* network : networks)
        if (network->state != NetworkState::Destroyed)
            for (Device* member : network->devices)
                if (member == device) return;
    device->destroyed = true;
    Enqueue(PARTY_STATE_CHANGE_TYPE_REMOTE_DEVICE_DESTROYED)->payload.remoteDeviceDestroyed.device = Handle(device);
    hellforge::Note("party link: device %s destroyed (no shared network)", device->node.c_str());
}

// A friend's machine leaves a network: its endpoints are destroyed, then REMOTE_DEVICE_LEFT_NETWORK.
void RemoveDevice(Network* network, const std::string& node, PARTY_DESTROYED_REASON reason)
{
    for (size_t i = 0; i < network->devices.size(); i++)
    {
        Device* device = network->devices[i];
        if (device->node != node) continue;
        for (Endpoint* endpoint : network->endpoints)
            if (endpoint->device == device && !endpoint->destroyed) EndpointDestroyed(network, endpoint, reason);
        network->devices.erase(network->devices.begin() + (ptrdiff_t)i);
        PARTY_REMOTE_DEVICE_LEFT_NETWORK_STATE_CHANGE& left = Enqueue(PARTY_STATE_CHANGE_TYPE_REMOTE_DEVICE_LEFT_NETWORK)->payload.remoteDeviceLeft;
        left.reason = reason;
        left.device = Handle(device);
        left.network = Handle(network);
        hellforge::Note("party link: device %s left network %s", node.c_str(), network->descriptor.networkIdentifier);
        RetireIfUnused(device);
        return;
    }
}

Endpoint* FindByUid(Network* network, uint16_t uid, bool local)
{
    for (Endpoint* endpoint : network->endpoints)
        if (!endpoint->destroyed && endpoint->created && endpoint->uniqueIdentifier == uid && (endpoint->device == nullptr) == local) return endpoint;
    return nullptr;
}

// A CONNECT_TO_NETWORK_COMPLETED for the network, its result still to be set.
PARTY_CONNECT_TO_NETWORK_COMPLETED_STATE_CHANGE& ConnectCompletion(Network* network)
{
    PARTY_CONNECT_TO_NETWORK_COMPLETED_STATE_CHANGE& connected = Enqueue(PARTY_STATE_CHANGE_TYPE_CONNECT_TO_NETWORK_COMPLETED)->payload.connectToNetwork;
    connected.networkDescriptor = network->descriptor;
    connected.asyncIdentifier = network->connectAsyncIdentifier;
    connected.network = Handle(network);
    return connected;
}

// A connected network's configuration, made available: NETWORK_CONFIGURATION_MADE_AVAILABLE.
void MakeConfigurationAvailable(Network* network, const PARTY_NETWORK_CONFIGURATION& configuration)
{
    network->configuration = configuration;
    network->configurationAvailable = true;
    PARTY_NETWORK_CONFIGURATION_MADE_AVAILABLE_STATE_CHANGE& available =
        Enqueue(PARTY_STATE_CHANGE_TYPE_NETWORK_CONFIGURATION_MADE_AVAILABLE)->payload.configurationMadeAvailable;
    available.network = Handle(network);
    available.networkConfiguration = &network->configuration;
}

// After a network's failed connect completion: its queued operations fail (a leave succeeds, after NETWORK_DESTROYED),
// and the network ends.
void FailQueued(Network* network)
{
    for (Operation& operation : network->pending)
        if (operation.type != PARTY_STATE_CHANGE_TYPE_LEAVE_NETWORK_COMPLETED) FailOperation(network, operation, PARTY_STATE_CHANGE_RESULT_INTERNET_CONNECTIVITY_ERROR);
    DestroyNetwork(network, PARTY_DESTROYED_REASON_CREATION_FAILED);
    for (Operation& operation : network->pending)
        if (operation.type == PARTY_STATE_CHANGE_TYPE_LEAVE_NETWORK_COMPLETED) LeaveCompleted(network, operation.asyncIdentifier);
    network->pending.clear();
}

// A friend's network that cannot be joined fails in the documented order, as offline.
void FailConnect(Network* network)
{
    PARTY_CONNECT_TO_NETWORK_COMPLETED_STATE_CHANGE& connected = ConnectCompletion(network);
    connected.result = PARTY_STATE_CHANGE_RESULT_INTERNET_CONNECTIVITY_ERROR;
    connected.errorDetail = unavailable;
    FailQueued(network);
}

// The friend's network this machine asked the backend to connect to, by the request's op (the network's address).
Network* ConnectingNetwork(const std::string& op)
{
    Network* network = nullptr;
    for (Network* candidate : networks)
        if (candidate->viaBackend && candidate->state == NetworkState::Connecting && std::to_string((unsigned long long)(uintptr_t)candidate) == op) network = candidate;
    return network;
}

PARTY_NETWORK_CONFIGURATION ReadConfiguration(hellforge::FrameReader& reader)
{
    PARTY_NETWORK_CONFIGURATION configuration{};
    configuration.maxUserCount = (uint32_t)reader.Int();
    configuration.maxDeviceCount = (uint32_t)reader.Int();
    configuration.maxUsersPerDeviceCount = (uint32_t)reader.Int();
    configuration.maxDevicesPerUserCount = (uint32_t)reader.Int();
    configuration.maxEndpointsPerDeviceCount = (uint32_t)reader.Int();
    configuration.directPeerConnectivityOptions = (PARTY_DIRECT_PEER_CONNECTIVITY_OPTIONS)reader.Int();
    return configuration;
}

// party.connected [op, result, network, configuration, endpoint block, count, (uid, node, entity id)...]: the host's
// answer to this machine's connect, with the endpoints already in the network.
void OnConnected(hellforge::FrameReader& reader)
{
    std::string op = reader.Text();
    long long result = reader.Int();
    std::string identifier = reader.Text();
    Network* network = ConnectingNetwork(op);
    if (!reader.ok || network == nullptr) return;
    if (result != 0)
    {
        hellforge::Note("party link: the host refused network %s", identifier.c_str());
        FailConnect(network);
        return;
    }
    PARTY_NETWORK_CONFIGURATION configuration = ReadConfiguration(reader);
    long long block = reader.Int();
    long long count = reader.Int();
    if (!reader.ok || count < 0 || count > 256) { FailConnect(network); return; }
    PARTY_CONNECT_TO_NETWORK_COMPLETED_STATE_CHANGE& connected = ConnectCompletion(network);
    connected.result = PARTY_STATE_CHANGE_RESULT_SUCCEEDED;
    network->state = NetworkState::Connected;
    network->endpointBlock = block > 0 && block <= 0xFFFF - 64 ? (uint16_t)block : 0;
    MakeConfigurationAvailable(network, configuration);
    hellforge::Note("party link: connected to network %s (%lld endpoints)", identifier.c_str(), count);
    for (long long i = 0; i < count && reader.ok; i++)
    {
        long long uid = reader.Int();
        std::string node = reader.Text(), entityId = reader.Text();
        if (reader.ok && uid > 0 && uid <= 0xFFFF) AddRemoteEndpoint(network, node, (uint16_t)uid, entityId);
    }
}

// party.endpointCreated [network, op, uid, node, entity id]: the host's id for an endpoint this machine asked for (op),
// or a friend's new endpoint (no op).
void OnEndpointCreated(Network* network, hellforge::FrameReader& reader)
{
    std::string op = reader.Text();
    long long uid = reader.Int();
    std::string node = reader.Text(), entityId = reader.Text();
    if (!reader.ok || uid <= 0 || uid > 0xFFFF) return;
    auto found = op.empty() ? network->waiting.end() : network->waiting.find(op);
    if (found == network->waiting.end())
    {
        if (op.empty()) AddRemoteEndpoint(network, node, (uint16_t)uid, entityId);
        return;
    }
    Operation operation = found->second;
    network->waiting.erase(found);
    operation.endpoint->created = true;
    operation.endpoint->uniqueIdentifier = (uint16_t)uid;
    EndpointCreated(network, operation);
}

void OnEndpointDestroyed(Network* network, hellforge::FrameReader& reader)
{
    long long uid = reader.Int();
    Endpoint* endpoint = reader.ok && uid > 0 && uid <= 0xFFFF ? FindByUid(network, (uint16_t)uid, false) : nullptr;
    if (endpoint != nullptr) EndpointDestroyed(network, endpoint, PARTY_DESTROYED_REASON_REQUESTED);
}

void OnDeviceJoined(Network* network, hellforge::FrameReader& reader)
{
    std::string node = reader.Text();
    if (reader.ok && !node.empty()) JoinDevice(network, node);
}

// "lost": the machine stopped answering. Anything else (or an older backend's frame without a reason) is a machine that
// left on purpose; HD2 will not reconnect to one reported as disconnected.
void OnDeviceLeft(Network* network, hellforge::FrameReader& reader)
{
    std::string node = reader.Text();
    bool named = reader.ok;
    std::string why = reader.Text();
    if (named) RemoveDevice(network, node, why == "lost" ? PARTY_DESTROYED_REASON_DISCONNECTED : PARTY_DESTROYED_REASON_REQUESTED);
}

// A message for no endpoint in particular: every local endpoint receives it.
void AddLocalReceivers(Network* network, std::vector<PARTY_ENDPOINT_HANDLE>& receivers)
{
    for (Endpoint* endpoint : network->endpoints)
        if (endpoint->device == nullptr && endpoint->created && !endpoint->destroyed) receivers.push_back(Handle(endpoint));
}

// The local endpoints a message's targets (their ids, comma-separated) name.
void AddTargetReceivers(Network* network, const std::string& targets, std::vector<PARTY_ENDPOINT_HANDLE>& receivers)
{
    size_t start = 0;
    while (start <= targets.size())
    {
        size_t comma = targets.find(',', start);
        if (comma == std::string::npos) comma = targets.size();
        long long uid = strtoll(targets.substr(start, comma - start).c_str(), nullptr, 10);
        Endpoint* endpoint = uid > 0 && uid <= 0xFFFF ? FindByUid(network, (uint16_t)uid, true) : nullptr;
        if (endpoint != nullptr) receivers.push_back(Handle(endpoint));
        start = comma + 1;
    }
}

// party.message [network, sender uid, options, targets, payload]: a friend's endpoint's message, for this machine's
// endpoints it names (or all of them).
void OnMessage(Network* network, hellforge::FrameReader& reader)
{
    long long sender = reader.Int();
    long long options = reader.Int();
    std::string targets = reader.Text(), payload = reader.Text();
    Endpoint* from = reader.ok && sender > 0 && sender <= 0xFFFF ? FindByUid(network, (uint16_t)sender, false) : nullptr;
    if (from == nullptr) return;
    StateChange* change = Enqueue(PARTY_STATE_CHANGE_TYPE_ENDPOINT_MESSAGE_RECEIVED);
    if (targets.empty()) AddLocalReceivers(network, change->receivers);
    else AddTargetReceivers(network, targets, change->receivers);
    if (change->receivers.empty())
    {
        queued.pop_back();
        Free(change);
        return;
    }
    change->message.assign(payload.begin(), payload.end());
    PARTY_ENDPOINT_MESSAGE_RECEIVED_STATE_CHANGE& received = change->payload.messageReceived;
    received.network = Handle(network);
    received.senderEndpoint = Handle(from);
    received.receiverEndpointCount = (uint32_t)change->receivers.size();
    received.receiverEndpoints = change->receivers.data();
    received.options = (PARTY_MESSAGE_RECEIVED_OPTIONS)(options & 0x3);
    received.messageSize = (uint32_t)change->message.size();
    received.messageBuffer = change->message.empty() ? nullptr : change->message.data();
}

// A local endpoint's message, to the backend when it is for a friend's endpoint or for everyone (no targets): the
// targets' ids, comma-separated, and the buffers' bytes, copied at once.
void ForwardMessage(const Endpoint* sender, uint32_t targetEndpointCount, const PARTY_ENDPOINT_HANDLE* targetEndpoints,
    PARTY_SEND_MESSAGE_OPTIONS options, uint32_t dataBufferCount, const PARTY_DATA_BUFFER* dataBuffers)
{
    std::string targets;
    bool remoteTarget = targetEndpointCount == 0;
    for (uint32_t i = 0; i < targetEndpointCount; i++)
    {
        Endpoint* target = Find(targetEndpoints[i]);
        if (target == nullptr || target->device == nullptr || !target->created) continue;
        if (!targets.empty()) targets += ',';
        targets += std::to_string(target->uniqueIdentifier);
        remoteTarget = true;
    }
    if (!remoteTarget) return;
    std::string payload;
    for (uint32_t i = 0; i < dataBufferCount; i++)
        if (dataBuffers[i].buffer != nullptr) payload.append(static_cast<const char*>(dataBuffers[i].buffer), dataBuffers[i].bufferByteCount);
    link.Send(hellforge::Frame("party.message").Add(sender->network->descriptor.networkIdentifier).Add((long long)sender->uniqueIdentifier)
        .Add((long long)options).Add(targets).Add(payload));
}

// Buffers a message was sent with without copying, handed back as the SDK does: DATA_BUFFERS_RETURNED.
void ReturnBuffers(const Endpoint* sender, PARTY_ENDPOINT_HANDLE endpoint, uint32_t dataBufferCount, const PARTY_DATA_BUFFER* dataBuffers,
    void* messageIdentifier)
{
    StateChange* change = Enqueue(PARTY_STATE_CHANGE_TYPE_DATA_BUFFERS_RETURNED);
    change->dataBuffers.assign(dataBuffers, dataBuffers + dataBufferCount);
    PARTY_DATA_BUFFERS_RETURNED_STATE_CHANGE& returned = change->payload.dataBuffersReturned;
    returned.network = Handle(sender->network);
    returned.localSenderEndpoint = endpoint;
    returned.dataBufferCount = dataBufferCount;
    returned.dataBuffers = change->dataBuffers.empty() ? nullptr : change->dataBuffers.data();
    returned.messageIdentifier = messageIdentifier;
}

void ApplyRemote(const hellforge::Frame& frame)
{
    hellforge::FrameReader reader(frame);
    if (frame.type == "party.connected")
    {
        OnConnected(reader);
        return;
    }
    std::string identifier = reader.Text();
    Network* network = FindNetwork(identifier);
    if (!reader.ok || network == nullptr) return;
    if (frame.type == "party.endpointCreated") OnEndpointCreated(network, reader);
    else if (frame.type == "party.endpointDestroyed") OnEndpointDestroyed(network, reader);
    else if (frame.type == "party.deviceJoined") OnDeviceJoined(network, reader);
    else if (frame.type == "party.deviceLeft") OnDeviceLeft(network, reader);
    else if (frame.type == "party.networkGone")
    {
        hellforge::Note("party link: network %s ended", identifier.c_str());
        DestroyNetwork(network, PARTY_DESTROYED_REASON_DISCONNECTED);
    }
    else if (frame.type == "party.message") OnMessage(network, reader);
}

// With the backend gone: a friend's network ends (one still connecting fails to connect), and friends' machines leave a
// network hosted here.
void LoseBackend(Network* network)
{
    if (network->state == NetworkState::Destroyed) return;
    if (network->viaBackend && network->state == NetworkState::Connecting) FailConnect(network);
    else if (network->viaBackend) DestroyNetwork(network, PARTY_DESTROYED_REASON_DISCONNECTED);
    else
    {
        std::vector<std::string> nodes;
        for (Device* device : network->devices) nodes.push_back(device->node);
        for (const std::string& node : nodes) RemoveDevice(network, node, PARTY_DESTROYED_REASON_DISCONNECTED);
    }
}

// Frames from the backend become state changes. If the backend went away, friends' networks end and friends leave
// networks hosted here, once.
void ProcessRemote()
{
    for (const hellforge::Frame& frame : link.Drain()) ApplyRemote(frame);
    if (!link.Broken() || remoteLost) return;
    remoteLost = true;
    hellforge::Note("party link: the backend went away");
    for (Network* network : networks) LoseBackend(network);
}

// A friend's network: the backend asks its host (once), and party.connected completes it (ApplyRemote).
void RequestConnect(Network* network)
{
    if (network->connectRequested) return;
    network->connectRequested = true;
    network->viaBackend = true;
    std::string host = HostEntity(network->descriptor);
    hellforge::Note("party link: connecting to network %s hosted by %s", network->descriptor.networkIdentifier, host.c_str());
    link.Send(hellforge::Frame("party.connect").Add(std::to_string((unsigned long long)(uintptr_t)network)).Add(network->descriptor.networkIdentifier).Add(host));
}

// A connecting network without the backend: one created here connects (completion, configuration, initial invitation);
// a friend's fails as offline, with its queued operations. False when it failed.
bool ConnectHere(Network* network)
{
    PARTY_CONNECT_TO_NETWORK_COMPLETED_STATE_CHANGE& connected = ConnectCompletion(network);
    if (network->hosted != nullptr)
    {
        connected.result = PARTY_STATE_CHANGE_RESULT_SUCCEEDED;
        network->state = NetworkState::Connected;
        MakeConfigurationAvailable(network, network->hosted->configuration);
        Invitation* initial = NewInvitation(network, nullptr, network->hosted->initialInvitation);
        PARTY_INVITATION_CREATED_STATE_CHANGE& created = Enqueue(PARTY_STATE_CHANGE_TYPE_INVITATION_CREATED)->payload.invitationCreated;
        created.network = Handle(network);
        created.invitation = Handle(initial);
        return true;
    }
    // A friend's network without the HellForge backend: nothing can reach it.
    connected.result = PARTY_STATE_CHANGE_RESULT_INTERNET_CONNECTIVITY_ERROR;
    connected.errorDetail = unavailable;
    memset(connected.networkDescriptor.regionName, 0, sizeof(connected.networkDescriptor.regionName));
    memset(connected.networkDescriptor.opaqueConnectionInformation, 0, sizeof(connected.networkDescriptor.opaqueConnectionInformation));
    FailQueued(network);
    return false;
}

// Runs before each delivery. A connecting network that was created here connects (completion, configuration,
// initial invitation); any other fails in the documented order: connect completion, its queued operations' failures,
// NETWORK_DESTROYED. Operations queued on a connected or destroyed network then run in call order.
void ResolveNetworks()
{
    for (Network* network : networks)
    {
        if (network->state == NetworkState::Connecting && network->hosted == nullptr && remote && !remoteLost)
        {
            RequestConnect(network);
            continue;
        }
        if (network->state == NetworkState::Connecting && !ConnectHere(network)) continue;
        std::vector<Operation> operations;
        operations.swap(network->pending);
        for (Operation& operation : operations) RunOperation(network, operation);
    }
    RefreshHandles();
}

}
