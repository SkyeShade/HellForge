// The Party stand-in's objects (party_shim.cpp): local users, networks, endpoints, invitations and devices, their handles,
// the state changes it queues for the game, and the helpers its exports share. Included by party_shim.cpp alone, before
// party_network.h: SdkShimGen reads party_shim.cpp for the exports written by hand, so they stay there.
#pragma once
#include "shim_common.h"
#include "link.h"
#include <Party_c.h>
#include <stdio.h>
#include <string.h>
#include <deque>
#include <map>
#include <string>
#include <vector>

namespace {

// The real library's error codes are not public; 0x10D0 is the SDK's own "failed loading party module", and the
// error-message function explains it.
constexpr PartyError unavailable = c_partyErrorFailedLoadingPartyModule;
const char unavailableMessage[] = "HellForge: PlayFab Party is replaced by the offline stand-in; this operation is not available.";
// The single region the stand-in reports; the game asks for regions by passing none to PartyCreateNewNetwork.
const char regionName[] = "HellForgeLocal";
// Serialized descriptors: "hellforge1;<network id>;<region>;<connection information as hex, trailing zeros dropped>".
const char serializedPrefix[] = "hellforge1;";
// The connection information of a network created here names its host: "hellforge-host:<entity id>".
const char hostPrefix[] = "hellforge-host:";

struct LocalUser
{
    char entityId[PARTY_MAX_ENTITY_ID_STRING_LENGTH + 1];
    char entityType[32];
    void* customContext;
};

// The local device, or a friend's machine (node: its SteamID64 in hex, as the backend names it).
struct Device
{
    void* customContext;
    std::string node;
    bool destroyed; // Reported REMOTE_DEVICE_DESTROYED; the handle stays valid, a return of the machine is a new device.
};

struct Network;

struct Endpoint
{
    Network* network;
    PARTY_LOCAL_USER_HANDLE localUser;
    void* customContext;
    uint16_t uniqueIdentifier; // Assigned when creation succeeds; 0 until then.
    bool created;
    bool destroyed;
    Device* device; // Null for this machine's endpoints.
    std::string entityId; // A remote endpoint's player.
};

struct InvitationSpec
{
    std::string identifier;
    PARTY_INVITATION_REVOCABILITY revocability;
    std::vector<std::string> entityIds;
};

struct Invitation
{
    Network* network;
    PARTY_LOCAL_USER_HANDLE creator; // Null for the network's initial invitation.
    void* customContext;
    bool destroyed;
    InvitationSpec spec;
    std::vector<PartyString> entityIdPointers;
    PARTY_INVITATION_CONFIGURATION configuration; // Points into spec.
};

// A network created by PartyCreateNewNetwork on this machine; PartyConnectToNetwork finds it by identifier.
struct HostedNetwork
{
    char networkIdentifier[PARTY_NETWORK_IDENTIFIER_STRING_LENGTH + 1];
    PARTY_NETWORK_CONFIGURATION configuration;
    InvitationSpec initialInvitation;
};

enum class NetworkState { Connecting, Connected, Destroyed };

// An asynchronous network operation, processed in call order at the next state-change delivery.
struct Operation
{
    PARTY_STATE_CHANGE_TYPE type;
    PARTY_LOCAL_USER_HANDLE localUser;
    void* asyncIdentifier;
    Endpoint* endpoint;
    Invitation* invitation;
    std::string invitationIdentifier;
};

// Network, endpoint and invitation objects live until PartyCleanup, so no handle the game still holds can dangle;
// each hosting attempt leaves a few hundred bytes behind.
struct Network
{
    PARTY_NETWORK_DESCRIPTOR descriptor;
    PARTY_NETWORK_CONFIGURATION configuration;
    bool configurationAvailable;
    const HostedNetwork* hosted; // Null when the network was not created on this machine.
    NetworkState state;
    void* customContext;
    void* connectAsyncIdentifier;
    uint16_t nextEndpointIdentifier;
    std::vector<Operation> pending;
    std::vector<PARTY_LOCAL_USER_HANDLE> authenticatedUsers;
    std::vector<Endpoint*> endpoints;
    std::vector<Invitation*> invitations;
    std::vector<PARTY_ENDPOINT_HANDLE> endpointHandles;
    std::vector<PARTY_INVITATION_HANDLE> invitationHandles;
    bool viaBackend; // A friend's network, joined through the HellForge backend.
    bool connectRequested;
    std::vector<Device*> devices; // Friends' machines in the network.
    std::vector<PARTY_DEVICE_HANDLE> deviceHandles;
    std::map<std::string, Operation> waiting; // Endpoint creations awaiting the host's endpoint id.
    uint16_t endpointBlock; // First id of the block the host gave this machine (0: ask the host per endpoint).
    uint16_t blockUsed;
};

union Payload
{
    PARTY_STATE_CHANGE base;
    PARTY_REGIONS_CHANGED_STATE_CHANGE regionsChanged;
    PARTY_CREATE_NEW_NETWORK_COMPLETED_STATE_CHANGE createNewNetwork;
    PARTY_DESTROY_LOCAL_USER_COMPLETED_STATE_CHANGE destroyLocalUser;
    PARTY_CONNECT_TO_NETWORK_COMPLETED_STATE_CHANGE connectToNetwork;
    PARTY_NETWORK_CONFIGURATION_MADE_AVAILABLE_STATE_CHANGE configurationMadeAvailable;
    PARTY_AUTHENTICATE_LOCAL_USER_COMPLETED_STATE_CHANGE authenticateLocalUser;
    PARTY_CREATE_ENDPOINT_COMPLETED_STATE_CHANGE createEndpoint;
    PARTY_ENDPOINT_CREATED_STATE_CHANGE endpointCreated;
    PARTY_DESTROY_ENDPOINT_COMPLETED_STATE_CHANGE destroyEndpoint;
    PARTY_ENDPOINT_DESTROYED_STATE_CHANGE endpointDestroyed;
    PARTY_CREATE_INVITATION_COMPLETED_STATE_CHANGE createInvitation;
    PARTY_INVITATION_CREATED_STATE_CHANGE invitationCreated;
    PARTY_INVITATION_DESTROYED_STATE_CHANGE invitationDestroyed;
    PARTY_NETWORK_DESTROYED_STATE_CHANGE networkDestroyed;
    PARTY_LEAVE_NETWORK_COMPLETED_STATE_CHANGE leaveNetwork;
    PARTY_DATA_BUFFERS_RETURNED_STATE_CHANGE dataBuffersReturned;
    PARTY_REMOTE_DEVICE_CREATED_STATE_CHANGE remoteDeviceCreated;
    PARTY_REMOTE_DEVICE_DESTROYED_STATE_CHANGE remoteDeviceDestroyed;
    PARTY_REMOTE_DEVICE_JOINED_NETWORK_STATE_CHANGE remoteDeviceJoined;
    PARTY_REMOTE_DEVICE_LEFT_NETWORK_STATE_CHANGE remoteDeviceLeft;
    PARTY_ENDPOINT_MESSAGE_RECEIVED_STATE_CHANGE messageReceived;
    PARTY_KICK_DEVICE_COMPLETED_STATE_CHANGE kickDevice;
    PARTY_KICK_USER_COMPLETED_STATE_CHANGE kickUser;
};

// A queued state change and the data its fields point to.
struct StateChange
{
    Payload payload;
    std::vector<PARTY_REGION> regions;
    std::vector<PARTY_DATA_BUFFER> dataBuffers;
    char invitationIdentifier[PARTY_MAX_INVITATION_IDENTIFIER_STRING_LENGTH + 1];
    LocalUser* released; // Freed once the game has finished processing the change.
    std::vector<uint8_t> message; // A received message's bytes and receivers, valid until the game finishes the change.
    std::vector<PARTY_ENDPOINT_HANDLE> receivers;
};

// State changes handed out by one PartyStartProcessingStateChanges call, freed by the matching finish call.
struct Batch
{
    std::vector<StateChange*> changes;
    std::vector<const PARTY_STATE_CHANGE*> pointers;
};

const char party = 0; // The library handle only has to be stable and non-null.
SRWLOCK lock = SRWLOCK_INIT;
Device localDevice;
std::vector<LocalUser*> localUsers;
std::vector<PARTY_LOCAL_USER_HANDLE> localUserHandles;
std::vector<HostedNetwork*> hostedNetworks;
std::vector<Network*> networks;
std::vector<PARTY_NETWORK_HANDLE> networkHandles;
std::vector<PARTY_REGION> regions; // Empty until the first PartyCreateNewNetwork that asks for regions.
std::vector<StateChange*> queued;
std::vector<Batch*> batches;
uint32_t identifierCounter;
const PARTY_DEVICE_HANDLE localDeviceList[1] = {reinterpret_cast<PARTY_DEVICE_HANDLE>(&localDevice)};
const PARTY_STATE_CHANGE* const noStateChanges[1] = {nullptr};
const PARTY_LOCAL_USER_HANDLE noLocalUsers[1] = {nullptr};
const PARTY_DEVICE_HANDLE noDevices[1] = {nullptr};
const PARTY_CHAT_CONTROL_HANDLE noChatControls[1] = {nullptr};
const PARTY_ENDPOINT_HANDLE noEndpoints[1] = {nullptr};
const PARTY_INVITATION_HANDLE noInvitations[1] = {nullptr};
const PARTY_NETWORK_HANDLE noNetworks[1] = {nullptr};
const PARTY_REGION noRegions[1] = {};

// Remote mode: the HellForge backend connects networks between friends.
hellforge::Link link;
bool remote;
bool remoteLost;
uint64_t operationCounter;
std::vector<Device*> remoteDevices; // Friends' machines; live until PartyCleanup so handles never dangle.
std::deque<std::string> kickedEntities; // The entity ids kick completions point to, kept until PartyCleanup.

class Locked
{
public:
    Locked() { AcquireSRWLockExclusive(&lock); }
    ~Locked() { ReleaseSRWLockExclusive(&lock); }
    Locked(const Locked&) = delete;
    Locked& operator=(const Locked&) = delete;
};

bool IsParty(PARTY_HANDLE handle) { return handle == reinterpret_cast<PARTY_HANDLE>(&party); }
PARTY_LOCAL_USER_HANDLE Handle(LocalUser* user) { return reinterpret_cast<PARTY_LOCAL_USER_HANDLE>(user); }
PARTY_NETWORK_HANDLE Handle(Network* network) { return reinterpret_cast<PARTY_NETWORK_HANDLE>(network); }
PARTY_ENDPOINT_HANDLE Handle(Endpoint* endpoint) { return reinterpret_cast<PARTY_ENDPOINT_HANDLE>(endpoint); }
PARTY_INVITATION_HANDLE Handle(Invitation* invitation) { return reinterpret_cast<PARTY_INVITATION_HANDLE>(invitation); }
PARTY_DEVICE_HANDLE DeviceHandle() { return localDeviceList[0]; }
PARTY_DEVICE_HANDLE Handle(Device* device) { return reinterpret_cast<PARTY_DEVICE_HANDLE>(device); }

Device* FindDevice(PARTY_DEVICE_HANDLE handle)
{
    if (handle == DeviceHandle()) return &localDevice;
    for (Device* device : remoteDevices)
        if (Handle(device) == handle) return device;
    return nullptr;
}

// The finders run under the lock and accept only handles this library issued.
LocalUser* Find(PARTY_LOCAL_USER_HANDLE handle)
{
    for (LocalUser* user : localUsers)
        if (Handle(user) == handle) return user;
    return nullptr;
}

Network* Find(PARTY_NETWORK_HANDLE handle)
{
    for (Network* network : networks)
        if (Handle(network) == handle) return network;
    return nullptr;
}

Endpoint* Find(PARTY_ENDPOINT_HANDLE handle)
{
    for (Network* network : networks)
        for (Endpoint* endpoint : network->endpoints)
            if (Handle(endpoint) == handle) return endpoint;
    return nullptr;
}

Invitation* Find(PARTY_INVITATION_HANDLE handle)
{
    for (Network* network : networks)
        for (Invitation* invitation : network->invitations)
            if (Handle(invitation) == handle) return invitation;
    return nullptr;
}

bool IsAuthenticated(const Network* network, PARTY_LOCAL_USER_HANDLE user)
{
    for (PARTY_LOCAL_USER_HANDLE authenticated : network->authenticatedUsers)
        if (authenticated == user) return true;
    return false;
}

// A network's handle lists: its live endpoints and invitations, and its devices once connected (this one first).
void RefreshNetworkHandles(Network* network)
{
    network->endpointHandles.clear();
    for (Endpoint* endpoint : network->endpoints)
        if (!endpoint->destroyed) network->endpointHandles.push_back(Handle(endpoint));
    network->invitationHandles.clear();
    for (Invitation* invitation : network->invitations)
        if (!invitation->destroyed) network->invitationHandles.push_back(Handle(invitation));
    network->deviceHandles.clear();
    if (network->state == NetworkState::Connected)
    {
        network->deviceHandles.push_back(DeviceHandle());
        for (Device* device : network->devices) network->deviceHandles.push_back(Handle(device));
    }
}

void RefreshHandles()
{
    localUserHandles.clear();
    for (LocalUser* user : localUsers) localUserHandles.push_back(Handle(user));
    networkHandles.clear();
    for (Network* network : networks)
    {
        if (network->state != NetworkState::Destroyed) networkHandles.push_back(Handle(network));
        RefreshNetworkHandles(network);
    }
}

bool CopyString(char* destination, size_t capacity, const char* source)
{
    if (source == nullptr) return false;
    size_t length = strlen(source);
    if (length >= capacity) return false;
    memcpy(destination, source, length + 1);
    return true;
}

// Local identifiers in the SDK's GUID-like format.
std::string NewIdentifier()
{
    char identifier[PARTY_NETWORK_IDENTIFIER_STRING_LENGTH + 1];
    uint64_t tick = GetTickCount64();
    snprintf(identifier, sizeof(identifier), "b75b75b7-%04lx-%04x-%04x-%012llx", GetCurrentProcessId() & 0xFFFF,
        ++identifierCounter & 0xFFFF, (unsigned)(tick & 0xFFFF), (unsigned long long)(tick & 0xFFFFFFFFFFFFull));
    return identifier;
}

InvitationSpec CopySpec(const PARTY_INVITATION_CONFIGURATION* configuration)
{
    InvitationSpec spec{};
    spec.revocability = PARTY_INVITATION_REVOCABILITY_ANYONE;
    if (configuration != nullptr)
    {
        if (configuration->identifier != nullptr && configuration->identifier[0] != '\0' &&
            strlen(configuration->identifier) <= PARTY_MAX_INVITATION_IDENTIFIER_STRING_LENGTH)
            spec.identifier = configuration->identifier;
        spec.revocability = configuration->revocability;
        for (uint32_t i = 0; configuration->entityIds != nullptr && i < configuration->entityIdCount; i++)
            if (configuration->entityIds[i] != nullptr) spec.entityIds.push_back(configuration->entityIds[i]);
    }
    if (spec.identifier.empty()) spec.identifier = NewIdentifier();
    return spec;
}

Invitation* NewInvitation(Network* network, PARTY_LOCAL_USER_HANDLE creator, InvitationSpec spec)
{
    Invitation* invitation = new Invitation{};
    invitation->network = network;
    invitation->creator = creator;
    invitation->spec = std::move(spec);
    for (const std::string& id : invitation->spec.entityIds) invitation->entityIdPointers.push_back(id.c_str());
    invitation->configuration.identifier = invitation->spec.identifier.c_str();
    invitation->configuration.revocability = invitation->spec.revocability;
    invitation->configuration.entityIdCount = (uint32_t)invitation->entityIdPointers.size();
    invitation->configuration.entityIds = invitation->entityIdPointers.empty() ? nullptr : invitation->entityIdPointers.data();
    network->invitations.push_back(invitation);
    return invitation;
}

StateChange* Enqueue(PARTY_STATE_CHANGE_TYPE type)
{
    StateChange* change = new StateChange{};
    memset(&change->payload, 0, sizeof(change->payload));
    change->payload.base.stateChangeType = type;
    queued.push_back(change);
    return change;
}

void EndpointDestroyed(Network* network, Endpoint* endpoint, PARTY_DESTROYED_REASON reason)
{
    PARTY_ENDPOINT_DESTROYED_STATE_CHANGE& destroyed = Enqueue(PARTY_STATE_CHANGE_TYPE_ENDPOINT_DESTROYED)->payload.endpointDestroyed;
    destroyed.network = Handle(network);
    destroyed.endpoint = Handle(endpoint);
    destroyed.reason = reason;
    destroyed.errorDetail = reason == PARTY_DESTROYED_REASON_REQUESTED ? c_partyErrorSuccess : unavailable;
    endpoint->destroyed = true;
}

void InvitationDestroyed(Network* network, Invitation* invitation, PARTY_DESTROYED_REASON reason)
{
    PARTY_INVITATION_DESTROYED_STATE_CHANGE& destroyed = Enqueue(PARTY_STATE_CHANGE_TYPE_INVITATION_DESTROYED)->payload.invitationDestroyed;
    destroyed.network = Handle(network);
    destroyed.invitation = Handle(invitation);
    destroyed.reason = reason;
    destroyed.errorDetail = reason == PARTY_DESTROYED_REASON_REQUESTED ? c_partyErrorSuccess : unavailable;
    invitation->destroyed = true;
}

void LeaveCompleted(Network* network, void* asyncIdentifier)
{
    PARTY_LEAVE_NETWORK_COMPLETED_STATE_CHANGE& completed = Enqueue(PARTY_STATE_CHANGE_TYPE_LEAVE_NETWORK_COMPLETED)->payload.leaveNetwork;
    completed.result = PARTY_STATE_CHANGE_RESULT_SUCCEEDED;
    completed.network = Handle(network);
    completed.asyncIdentifier = asyncIdentifier;
}

const char* Describe(const Payload& payload, uint32_t* value)
{
    switch (payload.base.stateChangeType)
    {
    case PARTY_STATE_CHANGE_TYPE_REGIONS_CHANGED: *value = payload.regionsChanged.result; return "REGIONS_CHANGED result";
    case PARTY_STATE_CHANGE_TYPE_CREATE_NEW_NETWORK_COMPLETED: *value = payload.createNewNetwork.result; return "CREATE_NEW_NETWORK_COMPLETED result";
    case PARTY_STATE_CHANGE_TYPE_DESTROY_LOCAL_USER_COMPLETED: *value = payload.destroyLocalUser.result; return "DESTROY_LOCAL_USER_COMPLETED result";
    case PARTY_STATE_CHANGE_TYPE_CONNECT_TO_NETWORK_COMPLETED: *value = payload.connectToNetwork.result; return "CONNECT_TO_NETWORK_COMPLETED result";
    case PARTY_STATE_CHANGE_TYPE_NETWORK_CONFIGURATION_MADE_AVAILABLE: *value = 0; return "NETWORK_CONFIGURATION_MADE_AVAILABLE";
    case PARTY_STATE_CHANGE_TYPE_AUTHENTICATE_LOCAL_USER_COMPLETED: *value = payload.authenticateLocalUser.result; return "AUTHENTICATE_LOCAL_USER_COMPLETED result";
    case PARTY_STATE_CHANGE_TYPE_CREATE_ENDPOINT_COMPLETED: *value = payload.createEndpoint.result; return "CREATE_ENDPOINT_COMPLETED result";
    case PARTY_STATE_CHANGE_TYPE_ENDPOINT_CREATED: *value = 0; return "ENDPOINT_CREATED";
    case PARTY_STATE_CHANGE_TYPE_DESTROY_ENDPOINT_COMPLETED: *value = payload.destroyEndpoint.result; return "DESTROY_ENDPOINT_COMPLETED result";
    case PARTY_STATE_CHANGE_TYPE_ENDPOINT_DESTROYED: *value = payload.endpointDestroyed.reason; return "ENDPOINT_DESTROYED reason";
    case PARTY_STATE_CHANGE_TYPE_CREATE_INVITATION_COMPLETED: *value = payload.createInvitation.result; return "CREATE_INVITATION_COMPLETED result";
    case PARTY_STATE_CHANGE_TYPE_INVITATION_CREATED: *value = 0; return "INVITATION_CREATED";
    case PARTY_STATE_CHANGE_TYPE_INVITATION_DESTROYED: *value = payload.invitationDestroyed.reason; return "INVITATION_DESTROYED reason";
    case PARTY_STATE_CHANGE_TYPE_NETWORK_DESTROYED: *value = payload.networkDestroyed.reason; return "NETWORK_DESTROYED reason";
    case PARTY_STATE_CHANGE_TYPE_LEAVE_NETWORK_COMPLETED: *value = payload.leaveNetwork.result; return "LEAVE_NETWORK_COMPLETED result";
    case PARTY_STATE_CHANGE_TYPE_DATA_BUFFERS_RETURNED: *value = payload.dataBuffersReturned.dataBufferCount; return "DATA_BUFFERS_RETURNED buffers";
    case PARTY_STATE_CHANGE_TYPE_KICK_DEVICE_COMPLETED: *value = payload.kickDevice.result; return "KICK_DEVICE_COMPLETED result";
    case PARTY_STATE_CHANGE_TYPE_KICK_USER_COMPLETED: *value = payload.kickUser.result; return "KICK_USER_COMPLETED result";
    case PARTY_STATE_CHANGE_TYPE_REMOTE_DEVICE_CREATED: *value = 0; return "REMOTE_DEVICE_CREATED";
    case PARTY_STATE_CHANGE_TYPE_REMOTE_DEVICE_DESTROYED: *value = 0; return "REMOTE_DEVICE_DESTROYED";
    case PARTY_STATE_CHANGE_TYPE_REMOTE_DEVICE_JOINED_NETWORK: *value = 0; return "REMOTE_DEVICE_JOINED_NETWORK";
    case PARTY_STATE_CHANGE_TYPE_REMOTE_DEVICE_LEFT_NETWORK: *value = payload.remoteDeviceLeft.reason; return "REMOTE_DEVICE_LEFT_NETWORK reason";
    case PARTY_STATE_CHANGE_TYPE_ENDPOINT_MESSAGE_RECEIVED: *value = payload.messageReceived.messageSize; return "ENDPOINT_MESSAGE_RECEIVED bytes";
    default: *value = payload.base.stateChangeType; return "state change type";
    }
}

void Free(StateChange* change)
{
    delete change->released;
    delete change;
}

PartyError NewLocalUser(PARTY_HANDLE handle, PartyString entityId, PartyString entityType, PARTY_LOCAL_USER_HANDLE* localUser)
{
    hellforge::Clear(localUser);
    if (!IsParty(handle) || localUser == nullptr || entityId == nullptr || entityId[0] == '\0') return unavailable;
    Locked locked;
    if (localUsers.size() >= PARTY_MAX_LOCAL_USERS_PER_DEVICE_COUNT) return unavailable;
    LocalUser* user = new LocalUser{};
    if (!CopyString(user->entityId, sizeof(user->entityId), entityId) ||
        !CopyString(user->entityType, sizeof(user->entityType), entityType))
    {
        delete user;
        return unavailable;
    }
    localUsers.push_back(user);
    RefreshHandles();
    *localUser = Handle(user);
    return c_partyErrorSuccess;
}

// A local user being destroyed leaves the local users, and every network it was authenticated in.
void ForgetLocalUser(LocalUser* user, PARTY_LOCAL_USER_HANDLE handle)
{
    for (size_t i = 0; i < localUsers.size(); i++)
        if (localUsers[i] == user) localUsers.erase(localUsers.begin() + (ptrdiff_t)i);
    for (Network* network : networks)
        for (size_t i = 0; i < network->authenticatedUsers.size(); i++)
            if (network->authenticatedUsers[i] == handle) network->authenticatedUsers.erase(network->authenticatedUsers.begin() + (ptrdiff_t)i);
}

// Writes a handle list owned by the library (or an empty one) to the game's outputs.
template <class H> PartyError List(const std::vector<H>& handles, const H* empty, uint32_t* count, const H** list)
{
    hellforge::Clear(count);
    hellforge::Clear(list);
    if (count == nullptr || list == nullptr) return unavailable;
    *count = (uint32_t)handles.size();
    *list = handles.empty() ? empty : handles.data();
    return c_partyErrorSuccess;
}

template <class T> PartyError GetContext(T* object, void** customContext)
{
    hellforge::Clear(customContext);
    if (object == nullptr || customContext == nullptr) return unavailable;
    *customContext = object->customContext;
    return c_partyErrorSuccess;
}

template <class T> PartyError SetContext(T* object, void* customContext)
{
    if (object == nullptr) return unavailable;
    object->customContext = customContext;
    return c_partyErrorSuccess;
}

int HexValue(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// Hex digits, an even count of them, as bytes into out (room checked by the caller): false at a character that is not one.
bool DecodeHex(const char* hex, uint8_t* out)
{
    for (size_t i = 0; hex[2 * i] != '\0'; i++)
    {
        int high = HexValue(hex[2 * i]), low = HexValue(hex[2 * i + 1]);
        if (high < 0 || low < 0) return false;
        out[i] = (uint8_t)(high << 4 | low);
    }
    return true;
}

// A serialized descriptor after its prefix, "<network id>;<region>;<connection information as hex>", into parsed: false
// when it is malformed or a part does not fit.
bool ParseDescriptor(const char* identifier, PARTY_NETWORK_DESCRIPTOR& parsed)
{
    const char* region = strchr(identifier, ';');
    const char* opaque = region != nullptr ? strchr(region + 1, ';') : nullptr;
    if (opaque == nullptr || (size_t)(region - identifier) >= sizeof(parsed.networkIdentifier) ||
        (size_t)(opaque - region - 1) >= sizeof(parsed.regionName) || strlen(opaque + 1) % 2 != 0 ||
        strlen(opaque + 1) / 2 > sizeof(parsed.opaqueConnectionInformation))
        return false;
    memcpy(parsed.networkIdentifier, identifier, (size_t)(region - identifier));
    memcpy(parsed.regionName, region + 1, (size_t)(opaque - region - 1));
    return DecodeHex(opaque + 1, parsed.opaqueConnectionInformation);
}

}
