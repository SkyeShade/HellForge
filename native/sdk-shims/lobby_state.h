// The lobby stand-in's state (lobby_shim.cpp): its lobbies, members and operations, the state changes it queues for the
// game, and how an operation completes on this machine. Included by lobby_shim.cpp alone, before lobby_remote.h:
// SdkShimGen reads lobby_shim.cpp for the exports written by hand, so they stay there.
#pragma once
#include "shim_common.h"
#include "link.h"
#include <PFMultiplayer.h>
#include <PFLobby.h>
#include <PFMatchmaking.h>
#include <stdio.h>
#include <string.h>
#include <deque>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace {

// "The requested lobby doesn't exist", from the SDK's error table (lobby research notes).
constexpr HRESULT lobbyNotFound = (HRESULT)0x89236226;
const char unavailable[] = "HellForge: PlayFab Multiplayer is replaced by the offline stand-in; this operation is not available.";
// "hellforge1-lobby;<lobby id>;<owner entity id>": the owner tells a friend's stand-in whose machine hosts the lobby.
const char connectionPrefix[] = "hellforge1-lobby;";

// Text handed to the game stays valid until PFMultiplayerUninitialize. The SDK only promises it until the next
// state-change processing, but HD2's engine keeps a lobby property's text (wrapper 0x8c6f00) beyond the call.
std::vector<char*> strings;

const char* Intern(const char* text)
{
    size_t length = strlen(text);
    char* copy = new char[length + 1];
    memcpy(copy, text, length + 1);
    strings.push_back(copy);
    return copy;
}

struct Member
{
    std::string id;
    std::string type;
    bool local;
    std::map<std::string, const char*> properties;
    std::vector<const char*> keys;
    PFEntityKey Key() const { return {id.c_str(), type.c_str()}; }
};

struct Lobby
{
    std::string id;
    std::string connectionString;
    uint32_t maxMemberCount;
    PFLobbyOwnerMigrationPolicy ownerMigrationPolicy;
    PFLobbyAccessPolicy accessPolicy;
    PFLobbyMembershipLock membershipLock;
    std::map<std::string, const char*> lobbyProperties;
    std::map<std::string, const char*> searchProperties;
    std::vector<const char*> lobbyKeys;
    std::vector<const char*> searchKeys;
    std::vector<Member*> members; // Current members.
    std::vector<PFEntityKey> memberKeys;
    std::vector<Member*> everyMember; // Owns every member ever added, so their keys stay valid.
    Member* owner;
    PFEntityKey ownerKey;
    void* customContext;
    bool joined;
};

struct PropertyChange
{
    std::string key;
    bool remove;
    std::string value;
};

enum class OperationType { CreateAndJoin, Join, PostUpdate, Leave, SendInvite, FindLobbies, ListenerChanged, ForceRemove };

// An asynchronous operation, completed in call order at the next state-change delivery. Its inputs are copied: the
// game's buffers are only valid during the call.
struct Operation
{
    OperationType type;
    Lobby* lobby;
    Member* member; // Creator, updating member, or the member leaving (null: every local member).
    void* asyncContext;
    std::string entityId; // Join, invite and listener operations name an entity that may not be a member.
    std::string entityType;
    std::string inviteeId; // also a force removal's target member
    std::string inviteeType;
    bool newOwner;
    std::string newOwnerId;
    bool maxMemberCountChanged;
    uint32_t maxMemberCount;
    bool accessPolicyChanged;
    PFLobbyAccessPolicy accessPolicy;
    bool membershipLockChanged;
    PFLobbyMembershipLock membershipLock;
    bool lobbyUpdate;
    std::vector<PropertyChange> lobbyProperties;
    std::vector<PropertyChange> searchProperties;
    bool memberUpdate;
    std::vector<PropertyChange> memberProperties;
};

union Payload
{
    PFLobbyStateChange base;
    PFLobbyCreateAndJoinLobbyCompletedStateChange createAndJoin;
    PFLobbyJoinLobbyCompletedStateChange join;
    PFLobbyMemberAddedStateChange memberAdded;
    PFLobbyMemberRemovedStateChange memberRemoved;
    PFLobbyLeaveLobbyCompletedStateChange leave;
    PFLobbyUpdatedStateChange updated;
    PFLobbyPostUpdateCompletedStateChange postUpdate;
    PFLobbyDisconnectingStateChange disconnecting;
    PFLobbyDisconnectedStateChange disconnected;
    PFLobbyFindLobbiesCompletedStateChange findLobbies;
    PFLobbyInviteListenerStatusChangedStateChange listenerChanged;
    PFLobbySendInviteCompletedStateChange sendInvite;
    PFLobbyForceRemoveMemberCompletedStateChange forceRemove;
    PFLobbyInviteReceivedStateChange inviteReceived;
};

// A search's results and the text they point to. A completion's results stay valid after its batch is finished, for the
// last few searches: the SDK promises them until FinishProcessingLobbyStateChanges, but HD2 keeps lobby text beyond its
// calls elsewhere (see strings), and interning every search's text would grow without end (a scan every 10 s).
struct SearchResults
{
    std::deque<std::string> texts; // a deque: pointers into its strings stay valid as it grows
    std::deque<PFEntityKey> owners;
    std::deque<std::vector<const char*>> keys, values;
    std::vector<PFLobbySearchResult> results;
    const char* Keep(const std::string& text) { texts.push_back(text); return texts.back().c_str(); }
};

// A queued state change and the data its fields point to.
struct StateChange
{
    Payload payload;
    PFEntityKey localUser;
    std::vector<const char*> lobbyKeys;
    std::vector<const char*> searchKeys;
    std::vector<const char*> memberKeys;
    std::vector<PFLobbyMemberUpdateSummary> memberUpdates;
    std::shared_ptr<SearchResults> search;
};

struct Batch
{
    std::vector<StateChange*> changes;
    std::vector<const PFLobbyStateChange*> pointers;
};

const char multiplayer = 0; // The library handle only has to be stable and non-null.
SRWLOCK lock = SRWLOCK_INIT;
std::vector<Lobby*> lobbies;
std::vector<Operation> pending;
std::vector<StateChange*> queued;
std::vector<Batch*> batches;
std::vector<std::string> listeningEntities;
std::map<std::string, std::string> listeningTypes; // a listening entity's type, for its InviteReceived
std::deque<std::shared_ptr<SearchResults>> recentSearches;
std::string lastFilter;
uint32_t identifierCounter;
const PFLobbyStateChange* const noLobbyStateChanges[1] = {nullptr};
const PFMatchmakingStateChange* const noMatchmakingStateChanges[1] = {nullptr};
const char* const noKeys[1] = {nullptr};
const PFEntityKey noMembers[1] = {};

// Remote mode: the backend's lobby service answers every request (lobby.result) and reports what changes.
hellforge::Link link;
bool remote;
bool remoteLost; // The backend went away and every lobby was told.
uint64_t operationCounter;
std::map<std::string, Operation> waiting; // Sent requests by operation id.

class Locked
{
public:
    Locked() { AcquireSRWLockExclusive(&lock); }
    ~Locked() { ReleaseSRWLockExclusive(&lock); }
    Locked(const Locked&) = delete;
    Locked& operator=(const Locked&) = delete;
};

bool IsMultiplayer(PFMultiplayerHandle handle) { return handle == reinterpret_cast<PFMultiplayerHandle>(&multiplayer); }
PFLobbyHandle Handle(Lobby* lobby) { return reinterpret_cast<PFLobbyHandle>(lobby); }

bool ValidKey(const PFEntityKey* key) { return key != nullptr && key->id != nullptr && key->id[0] != '\0' && key->type != nullptr; }

// The finders run under the lock and accept only handles this library issued.
Lobby* Find(PFLobbyHandle handle)
{
    for (Lobby* lobby : lobbies)
        if (Handle(lobby) == handle) return lobby;
    return nullptr;
}

Member* FindMember(const Lobby* lobby, const PFEntityKey* key)
{
    if (lobby == nullptr || !ValidKey(key)) return nullptr;
    for (Member* member : lobby->members)
        if (member->id == key->id) return member;
    return nullptr;
}

std::string NewIdentifier()
{
    char identifier[40];
    uint64_t tick = GetTickCount64();
    snprintf(identifier, sizeof(identifier), "b75b10bb-%04lx-%04x-%04x-%012llx", GetCurrentProcessId() & 0xFFFF,
        ++identifierCounter & 0xFFFF, (unsigned)(tick & 0xFFFF), (unsigned long long)(tick & 0xFFFFFFFFFFFFull));
    return identifier;
}

void RefreshKeys(const std::map<std::string, const char*>& properties, std::vector<const char*>& keys)
{
    keys.clear();
    for (const auto& property : properties) keys.push_back(property.first.c_str());
}

void RefreshMembers(Lobby* lobby)
{
    lobby->memberKeys.clear();
    for (Member* member : lobby->members) lobby->memberKeys.push_back(member->Key());
    lobby->ownerKey = lobby->owner != nullptr ? lobby->owner->Key() : PFEntityKey{};
}

// Returns false when the game passed inconsistent arrays.
bool CopyProperties(uint32_t count, const char* const* keys, const char* const* values, std::vector<PropertyChange>& changes)
{
    if (count > 0 && (keys == nullptr || values == nullptr)) return false;
    for (uint32_t i = 0; i < count; i++)
    {
        if (keys[i] == nullptr || keys[i][0] == '\0') return false;
        // A null value deletes the key.
        changes.push_back({keys[i], values[i] == nullptr, values[i] != nullptr ? values[i] : ""});
    }
    return true;
}

void ApplyProperties(const std::vector<PropertyChange>& changes, std::map<std::string, const char*>& properties, std::vector<const char*>& keys)
{
    for (const PropertyChange& change : changes)
    {
        if (change.remove) properties.erase(change.key);
        else properties[change.key] = Intern(change.value.c_str());
    }
    RefreshKeys(properties, keys);
}

// Records what the game publishes, which a joining friend's game reads (Party descriptor, invite id, platform lobby).
void LogProperties(const char* scope, const std::string& owner, const std::vector<PropertyChange>& changes)
{
    for (const PropertyChange& change : changes)
        if (change.remove) hellforge::Note("%s property of %s: \"%s\" removed", scope, owner.c_str(), change.key.c_str());
        else hellforge::Note("%s property of %s: \"%s\" = \"%.300s\"", scope, owner.c_str(), change.key.c_str(), change.value.c_str());
}

Member* NewMember(Lobby* lobby, const std::string& id, const std::string& type, bool local)
{
    Member* member = new Member{};
    member->id = id;
    member->type = type;
    member->local = local;
    lobby->everyMember.push_back(member);
    lobby->members.push_back(member);
    RefreshMembers(lobby);
    return member;
}

StateChange* Enqueue(PFLobbyStateChangeType type)
{
    StateChange* change = new StateChange{};
    memset(&change->payload, 0, sizeof(change->payload));
    change->payload.base.stateChangeType = type;
    queued.push_back(change);
    return change;
}

PFEntityKey InternKey(const std::string& id, const std::string& type) { return {Intern(id.c_str()), Intern(type.c_str())}; }

// An Updated state change's lobby and search property changes: applied to the lobby, and their keys named in it.
void UpdateLobbyProperties(Lobby* lobby, StateChange* update, const std::vector<PropertyChange>& lobbyChanges,
    const std::vector<PropertyChange>& searchChanges)
{
    PFLobbyUpdatedStateChange& updated = update->payload.updated;
    ApplyProperties(lobbyChanges, lobby->lobbyProperties, lobby->lobbyKeys);
    ApplyProperties(searchChanges, lobby->searchProperties, lobby->searchKeys);
    for (const PropertyChange& changed : lobbyChanges) update->lobbyKeys.push_back(Intern(changed.key.c_str()));
    for (const PropertyChange& changed : searchChanges) update->searchKeys.push_back(Intern(changed.key.c_str()));
    updated.updatedLobbyPropertyCount = (uint32_t)update->lobbyKeys.size();
    updated.updatedLobbyPropertyKeys = update->lobbyKeys.empty() ? nullptr : update->lobbyKeys.data();
    updated.updatedSearchPropertyCount = (uint32_t)update->searchKeys.size();
    updated.updatedSearchPropertyKeys = update->searchKeys.empty() ? nullptr : update->searchKeys.data();
}

// An Updated state change's one member update: its property changes applied to the member and summarized in it.
void UpdateMemberProperties(Member* member, StateChange* update, const std::vector<PropertyChange>& changes)
{
    PFLobbyUpdatedStateChange& updated = update->payload.updated;
    ApplyProperties(changes, member->properties, member->keys);
    for (const PropertyChange& changed : changes) update->memberKeys.push_back(Intern(changed.key.c_str()));
    PFLobbyMemberUpdateSummary summary{};
    summary.member = member->Key();
    summary.updatedMemberPropertyCount = (uint32_t)update->memberKeys.size();
    summary.updatedMemberPropertyKeys = update->memberKeys.empty() ? nullptr : update->memberKeys.data();
    update->memberUpdates.push_back(summary);
    updated.memberUpdateCount = 1;
    updated.memberUpdates = update->memberUpdates.data();
}

void CompleteCreateAndJoin(const Operation& operation)
{
    Lobby* lobby = operation.lobby;
    // The SDK reports the creator's MemberAdded before the completion.
    lobby->joined = true;
    PFLobbyMemberAddedStateChange& added = Enqueue(PFLobbyStateChangeType::MemberAdded)->payload.memberAdded;
    added.lobby = Handle(lobby);
    added.member = operation.member->Key();
    PFLobbyCreateAndJoinLobbyCompletedStateChange& completed = Enqueue(PFLobbyStateChangeType::CreateAndJoinLobbyCompleted)->payload.createAndJoin;
    completed.result = S_OK;
    completed.asyncContext = operation.asyncContext;
    completed.lobby = Handle(lobby);
}

void CompleteJoin(const Operation& operation)
{
    // A friend's lobby: nothing can reach it yet.
    PFLobbyJoinLobbyCompletedStateChange& completed = Enqueue(PFLobbyStateChangeType::JoinLobbyCompleted)->payload.join;
    completed.result = lobbyNotFound;
    completed.newMember = InternKey(operation.entityId, operation.entityType);
    completed.asyncContext = operation.asyncContext;
    completed.lobby = Handle(operation.lobby);
}

// Whether an update may be applied: the updating member is in the joined lobby, only the owner changes the lobby itself,
// and a new owner (found into *newOwner) is a member.
HRESULT PostUpdateResult(const Operation& operation, const PFEntityKey& localUser, Member** newOwner)
{
    Lobby* lobby = operation.lobby;
    bool member = lobby->joined && FindMember(lobby, &localUser) != nullptr;
    if (operation.newOwner)
    {
        PFEntityKey key{operation.newOwnerId.c_str(), ""};
        *newOwner = FindMember(lobby, &key);
    }
    if (!member) return lobbyNotFound;
    if (operation.lobbyUpdate && lobby->owner != operation.member) return E_ACCESSDENIED;
    if (operation.newOwner && *newOwner == nullptr) return E_INVALIDARG;
    return S_OK;
}

// An accepted update's Updated state change: the owner, the lobby's settings and properties, the member's properties.
void ApplyPostUpdate(const Operation& operation, Member* newOwner)
{
    Lobby* lobby = operation.lobby;
    StateChange* update = Enqueue(PFLobbyStateChangeType::Updated);
    PFLobbyUpdatedStateChange& updated = update->payload.updated;
    updated.lobby = Handle(lobby);
    if (operation.newOwner)
    {
        lobby->owner = newOwner;
        updated.ownerUpdated = true;
    }
    if (operation.maxMemberCountChanged)
    {
        lobby->maxMemberCount = operation.maxMemberCount;
        updated.maxMembersUpdated = true;
    }
    if (operation.accessPolicyChanged)
    {
        lobby->accessPolicy = operation.accessPolicy;
        updated.accessPolicyUpdated = true;
    }
    if (operation.membershipLockChanged)
    {
        lobby->membershipLock = operation.membershipLock;
        updated.membershipLockUpdated = true;
    }
    UpdateLobbyProperties(lobby, update, operation.lobbyProperties, operation.searchProperties);
    if (operation.memberUpdate) UpdateMemberProperties(operation.member, update, operation.memberProperties);
    RefreshMembers(lobby);
}

void CompletePostUpdate(const Operation& operation)
{
    StateChange* change = Enqueue(PFLobbyStateChangeType::PostUpdateCompleted);
    PFLobbyPostUpdateCompletedStateChange& completed = change->payload.postUpdate;
    completed.lobby = Handle(operation.lobby);
    completed.localUser = operation.member->Key();
    completed.asyncContext = operation.asyncContext;
    Member* newOwner = nullptr;
    completed.result = PostUpdateResult(operation, completed.localUser, &newOwner);
    if (completed.result == S_OK) ApplyPostUpdate(operation, newOwner);
}

void CompleteForceRemove(const Operation& operation)
{
    // Without the backend no other machine's member can be in a lobby, and a local member is never kicked.
    PFLobbyForceRemoveMemberCompletedStateChange& completed = Enqueue(PFLobbyStateChangeType::ForceRemoveMemberCompleted)->payload.forceRemove;
    completed.result = E_INVALIDARG;
    completed.lobby = Handle(operation.lobby);
    completed.targetMember = InternKey(operation.inviteeId, operation.inviteeType);
    completed.asyncContext = operation.asyncContext;
}

void CompleteSendInvite(const Operation& operation)
{
    // Without the backend no invitation can leave this machine.
    PFLobbySendInviteCompletedStateChange& completed = Enqueue(PFLobbyStateChangeType::SendInviteCompleted)->payload.sendInvite;
    completed.result = E_NOTIMPL;
    completed.lobby = Handle(operation.lobby);
    completed.sender = InternKey(operation.entityId, operation.entityType);
    completed.invitee = InternKey(operation.inviteeId, operation.inviteeType);
    completed.asyncContext = operation.asyncContext;
}

void CompleteFindLobbies(const Operation& operation)
{
    PFLobbyFindLobbiesCompletedStateChange& completed = Enqueue(PFLobbyStateChangeType::FindLobbiesCompleted)->payload.findLobbies;
    completed.result = S_OK;
    completed.searchingEntity = InternKey(operation.entityId, operation.entityType);
    completed.asyncContext = operation.asyncContext;
}

void LeaveLocally(Operation& operation);

void Complete(Operation& operation)
{
    switch (operation.type)
    {
    case OperationType::CreateAndJoin: CompleteCreateAndJoin(operation); break;
    case OperationType::Join: CompleteJoin(operation); break;
    case OperationType::PostUpdate: CompletePostUpdate(operation); break;
    case OperationType::Leave: LeaveLocally(operation); break;
    case OperationType::ForceRemove: CompleteForceRemove(operation); break;
    case OperationType::SendInvite: CompleteSendInvite(operation); break;
    case OperationType::FindLobbies: CompleteFindLobbies(operation); break;
    case OperationType::ListenerChanged:
        Enqueue(PFLobbyStateChangeType::InviteListenerStatusChanged)->payload.listenerChanged.listeningEntity =
            InternKey(operation.entityId, operation.entityType);
        break;
    }
}

// The leaving local members (one, or every local one) taken out of the lobby, each with MemberRemoved.
void RemoveLeaving(Lobby* lobby, const Member* only)
{
    std::vector<Member*> leaving;
    for (Member* member : lobby->members)
        if (member->local && (only == nullptr || member == only)) leaving.push_back(member);
    for (Member* member : leaving)
    {
        for (size_t i = 0; i < lobby->members.size(); i++)
            if (lobby->members[i] == member) lobby->members.erase(lobby->members.begin() + (ptrdiff_t)i);
        PFLobbyMemberRemovedStateChange& removed = Enqueue(PFLobbyStateChangeType::MemberRemoved)->payload.memberRemoved;
        removed.lobby = Handle(lobby);
        removed.member = member->Key();
        removed.reason = PFLobbyMemberRemovedReason::LocalUserLeftLobby;
    }
}

// A joined lobby with no local member left ends: Disconnecting (NoLocalMembers), then Disconnected.
void DisconnectIfNoLocalMembers(Lobby* lobby)
{
    bool localMembers = false;
    for (Member* member : lobby->members) localMembers = localMembers || member->local;
    if (!lobby->joined || localMembers) return;
    lobby->joined = false;
    PFLobbyDisconnectingStateChange& disconnecting = Enqueue(PFLobbyStateChangeType::Disconnecting)->payload.disconnecting;
    disconnecting.lobby = Handle(lobby);
    disconnecting.reason = PFLobbyDisconnectingReason::NoLocalMembers;
    Enqueue(PFLobbyStateChangeType::Disconnected)->payload.disconnected.lobby = Handle(lobby);
}

// The local member (or every local member) leaves: MemberRemoved, LeaveLobbyCompleted, and when no local member is
// left, Disconnecting and Disconnected.
void LeaveLocally(Operation& operation)
{
    Lobby* lobby = operation.lobby;
    RemoveLeaving(lobby, operation.member);
    RefreshMembers(lobby);
    StateChange* change = Enqueue(PFLobbyStateChangeType::LeaveLobbyCompleted);
    change->payload.leave.lobby = Handle(lobby);
    if (operation.member != nullptr)
    {
        change->localUser = operation.member->Key();
        change->payload.leave.localUser = &change->localUser;
    }
    change->payload.leave.asyncContext = operation.asyncContext;
    DisconnectIfNoLocalMembers(lobby);
}

const char* Describe(const Payload& payload, uint32_t* value)
{
    switch (payload.base.stateChangeType)
    {
    case PFLobbyStateChangeType::CreateAndJoinLobbyCompleted: *value = (uint32_t)payload.createAndJoin.result; return "CreateAndJoinLobbyCompleted result";
    case PFLobbyStateChangeType::JoinLobbyCompleted: *value = (uint32_t)payload.join.result; return "JoinLobbyCompleted result";
    case PFLobbyStateChangeType::MemberAdded: *value = 0; return "MemberAdded";
    case PFLobbyStateChangeType::MemberRemoved: *value = (uint32_t)payload.memberRemoved.reason; return "MemberRemoved reason";
    case PFLobbyStateChangeType::LeaveLobbyCompleted: *value = 0; return "LeaveLobbyCompleted";
    case PFLobbyStateChangeType::Updated: *value = payload.updated.updatedLobbyPropertyCount; return "Updated lobby properties";
    case PFLobbyStateChangeType::PostUpdateCompleted: *value = (uint32_t)payload.postUpdate.result; return "PostUpdateCompleted result";
    case PFLobbyStateChangeType::Disconnecting: *value = (uint32_t)payload.disconnecting.reason; return "Disconnecting reason";
    case PFLobbyStateChangeType::Disconnected: *value = 0; return "Disconnected";
    case PFLobbyStateChangeType::FindLobbiesCompleted: *value = (uint32_t)payload.findLobbies.result; return "FindLobbiesCompleted result";
    case PFLobbyStateChangeType::InviteListenerStatusChanged: *value = 0; return "InviteListenerStatusChanged";
    case PFLobbyStateChangeType::SendInviteCompleted: *value = (uint32_t)payload.sendInvite.result; return "SendInviteCompleted result";
    case PFLobbyStateChangeType::ForceRemoveMemberCompleted: *value = (uint32_t)payload.forceRemove.result; return "ForceRemoveMemberCompleted result";
    case PFLobbyStateChangeType::InviteReceived: *value = 0; return "InviteReceived";
    default: *value = (uint32_t)payload.base.stateChangeType; return "state change type";
    }
}

}
