// The lobby stand-in through the local HellForge backend's lobby service (lobby_shim.cpp, link.h): the requests it
// sends, and the backend's answers and events as state changes. Included by lobby_shim.cpp alone, after lobby_state.h.
#pragma once
#include "lobby_state.h"

namespace {

void AddChanges(hellforge::Frame& frame, const std::vector<PropertyChange>& changes)
{
    frame.Add((long long)changes.size());
    for (const PropertyChange& change : changes) frame.Add(change.key).Add(change.remove ? "remove" : "set").Add(change.value);
}

std::map<std::string, std::string> SetValues(const std::vector<PropertyChange>& changes)
{
    std::map<std::string, std::string> values;
    for (const PropertyChange& change : changes)
        if (!change.remove) values[change.key] = change.value;
    return values;
}

std::vector<PropertyChange> FromChanges(const std::vector<hellforge::FrameReader::Change>& changes)
{
    std::vector<PropertyChange> converted;
    for (const auto& change : changes) converted.push_back({change.key, change.remove, change.value});
    return converted;
}

// Sends a request; its answer completes the operation. A request the backend cannot receive fails at the next delivery.
void Request(hellforge::Frame frame, Operation operation)
{
    std::string id = "op" + std::to_string(++operationCounter);
    frame.fields.insert(frame.fields.begin(), id);
    waiting[id] = std::move(operation);
    if (!link.Send(frame)) hellforge::Note("lobby link: could not send %s", frame.type.c_str());
}

Lobby* FindById(const std::string& id)
{
    for (Lobby* lobby : lobbies)
        if (lobby->id == id && lobby->joined) return lobby;
    return nullptr;
}

// The backend's properties, interned, as the whole of a lobby's or member's properties.
void SetProperties(const std::map<std::string, std::string>& given, std::map<std::string, const char*>& properties,
    std::vector<const char*>& keys)
{
    properties.clear();
    for (const auto& property : given) properties[property.first] = Intern(property.second.c_str());
    RefreshKeys(properties, keys);
}

// One member of a snapshot: a member seen before keeps its object (and its key's text), a new one is added.
Member* SnapshotMember(Lobby* lobby, const std::string& memberId, const std::string& memberType, const std::string& localId)
{
    Member* member = nullptr;
    for (Member* known : lobby->everyMember)
        if (known->id == memberId) member = known;
    if (member == nullptr) return NewMember(lobby, memberId, memberType, memberId == localId);
    lobby->members.push_back(member);
    return member;
}

// A lobby as the backend describes it: the snapshot in a create or join answer.
bool ApplySnapshot(Lobby* lobby, hellforge::FrameReader& reader, const std::string& localId)
{
    std::string id = reader.Text(), connection = reader.Text(), ownerId = reader.Text();
    long long maxMembers = reader.Int(), access = reader.Int(), migration = reader.Int(), lockState = reader.Int();
    std::map<std::string, std::string> lobbyProperties = reader.Properties(), searchProperties = reader.Properties();
    long long count = reader.Int();
    if (!reader.ok || count < 1 || count > 64) return false;
    lobby->id = id;
    lobby->connectionString = connection;
    lobby->maxMemberCount = (uint32_t)maxMembers;
    lobby->accessPolicy = (PFLobbyAccessPolicy)access;
    lobby->ownerMigrationPolicy = (PFLobbyOwnerMigrationPolicy)migration;
    lobby->membershipLock = (PFLobbyMembershipLock)lockState;
    SetProperties(lobbyProperties, lobby->lobbyProperties, lobby->lobbyKeys);
    SetProperties(searchProperties, lobby->searchProperties, lobby->searchKeys);
    lobby->members.clear();
    lobby->owner = nullptr;
    for (long long i = 0; i < count && reader.ok; i++)
    {
        std::string memberId = reader.Text(), memberType = reader.Text();
        std::map<std::string, std::string> properties = reader.Properties();
        Member* member = SnapshotMember(lobby, memberId, memberType, localId);
        SetProperties(properties, member->properties, member->keys);
        if (memberId == ownerId) lobby->owner = member;
    }
    RefreshMembers(lobby);
    return reader.ok;
}

void Finish(Operation& operation, HRESULT result)
{
    Lobby* lobby = operation.lobby;
    switch (operation.type)
    {
    case OperationType::CreateAndJoin:
    {
        if (SUCCEEDED(result))
        {
            lobby->joined = true;
            PFLobbyMemberAddedStateChange& added = Enqueue(PFLobbyStateChangeType::MemberAdded)->payload.memberAdded;
            added.lobby = Handle(lobby);
            added.member = operation.member->Key();
        }
        PFLobbyCreateAndJoinLobbyCompletedStateChange& completed = Enqueue(PFLobbyStateChangeType::CreateAndJoinLobbyCompleted)->payload.createAndJoin;
        completed.result = result;
        completed.asyncContext = operation.asyncContext;
        completed.lobby = Handle(lobby);
        break;
    }
    case OperationType::Join:
    {
        if (SUCCEEDED(result))
        {
            lobby->joined = true;
            for (Member* member : lobby->members)
            {
                PFLobbyMemberAddedStateChange& added = Enqueue(PFLobbyStateChangeType::MemberAdded)->payload.memberAdded;
                added.lobby = Handle(lobby);
                added.member = member->Key();
            }
        }
        PFLobbyJoinLobbyCompletedStateChange& completed = Enqueue(PFLobbyStateChangeType::JoinLobbyCompleted)->payload.join;
        completed.result = result;
        completed.newMember = InternKey(operation.entityId, operation.entityType);
        completed.asyncContext = operation.asyncContext;
        completed.lobby = Handle(lobby);
        if (FAILED(result))
        {
            // A refused join also ends the lobby object (NoLocalMembers): with only the failed completion, HD2 kept
            // retrying the join every 20 s until "lobby timeout" (two-PC test with different mods).
            PFLobbyDisconnectingStateChange& disconnecting = Enqueue(PFLobbyStateChangeType::Disconnecting)->payload.disconnecting;
            disconnecting.lobby = Handle(lobby);
            disconnecting.reason = PFLobbyDisconnectingReason::NoLocalMembers;
            Enqueue(PFLobbyStateChangeType::Disconnected)->payload.disconnected.lobby = Handle(lobby);
        }
        break;
    }
    case OperationType::PostUpdate:
    {
        PFLobbyPostUpdateCompletedStateChange& completed = Enqueue(PFLobbyStateChangeType::PostUpdateCompleted)->payload.postUpdate;
        completed.result = result;
        completed.lobby = Handle(lobby);
        completed.localUser = operation.member->Key();
        completed.asyncContext = operation.asyncContext;
        break;
    }
    case OperationType::Leave:
        // Leaving always succeeds locally, whatever the backend answered.
        LeaveLocally(operation);
        break;
    case OperationType::ForceRemove:
    {
        // The backend's lobby.memberRemoved for the kicked member came first, as the SDK orders them.
        PFLobbyForceRemoveMemberCompletedStateChange& completed = Enqueue(PFLobbyStateChangeType::ForceRemoveMemberCompleted)->payload.forceRemove;
        completed.result = result;
        completed.lobby = Handle(lobby);
        completed.targetMember = InternKey(operation.inviteeId, operation.inviteeType);
        completed.asyncContext = operation.asyncContext;
        break;
    }
    case OperationType::SendInvite:
    {
        PFLobbySendInviteCompletedStateChange& completed = Enqueue(PFLobbyStateChangeType::SendInviteCompleted)->payload.sendInvite;
        completed.result = result;
        completed.lobby = Handle(lobby);
        completed.sender = InternKey(operation.entityId, operation.entityType);
        completed.invitee = InternKey(operation.inviteeId, operation.inviteeType);
        completed.asyncContext = operation.asyncContext;
        break;
    }
    case OperationType::FindLobbies:
    {
        // The backend went away during a search.
        PFLobbyFindLobbiesCompletedStateChange& completed = Enqueue(PFLobbyStateChangeType::FindLobbiesCompleted)->payload.findLobbies;
        completed.result = result;
        completed.searchingEntity = InternKey(operation.entityId, operation.entityType);
        completed.asyncContext = operation.asyncContext;
        break;
    }
    default:
        break;
    }
}

void Disconnect(Lobby* lobby, PFLobbyDisconnectingReason reason)
{
    if (!lobby->joined) return;
    lobby->joined = false;
    PFLobbyDisconnectingStateChange& disconnecting = Enqueue(PFLobbyStateChangeType::Disconnecting)->payload.disconnecting;
    disconnecting.lobby = Handle(lobby);
    disconnecting.reason = reason;
    Enqueue(PFLobbyStateChangeType::Disconnected)->payload.disconnected.lobby = Handle(lobby);
}

// The settings a lobby.updated names, each as text and empty when unchanged: the owner, max members, access, lock.
void UpdateSettings(Lobby* lobby, PFLobbyUpdatedStateChange& updated, const std::string& ownerId, const std::string& maxMembers,
    const std::string& access, const std::string& lockState)
{
    if (!ownerId.empty())
        for (Member* member : lobby->members)
            if (member->id == ownerId)
            {
                lobby->owner = member;
                updated.ownerUpdated = true;
            }
    if (!maxMembers.empty())
    {
        lobby->maxMemberCount = (uint32_t)strtoul(maxMembers.c_str(), nullptr, 10);
        updated.maxMembersUpdated = true;
    }
    if (!access.empty())
    {
        lobby->accessPolicy = (PFLobbyAccessPolicy)strtoul(access.c_str(), nullptr, 10);
        updated.accessPolicyUpdated = true;
    }
    if (!lockState.empty())
    {
        lobby->membershipLock = (PFLobbyMembershipLock)strtoul(lockState.c_str(), nullptr, 10);
        updated.membershipLockUpdated = true;
    }
}

void ApplyUpdate(Lobby* lobby, hellforge::FrameReader& reader)
{
    std::string ownerId = reader.Text(), maxMembers = reader.Text(), access = reader.Text(), lockState = reader.Text();
    std::vector<PropertyChange> lobbyChanges = FromChanges(reader.Changes()), searchChanges = FromChanges(reader.Changes());
    long long memberUpdates = reader.Int();
    std::string memberId = memberUpdates > 0 ? reader.Text() : "";
    std::vector<PropertyChange> memberChanges = memberUpdates > 0 ? FromChanges(reader.Changes()) : std::vector<PropertyChange>();
    if (!reader.ok) return;
    StateChange* update = Enqueue(PFLobbyStateChangeType::Updated);
    PFLobbyUpdatedStateChange& updated = update->payload.updated;
    updated.lobby = Handle(lobby);
    UpdateSettings(lobby, updated, ownerId, maxMembers, access, lockState);
    UpdateLobbyProperties(lobby, update, lobbyChanges, searchChanges);
    Member* member = nullptr;
    for (Member* candidate : lobby->members)
        if (candidate->id == memberId) member = candidate;
    if (member != nullptr) UpdateMemberProperties(member, update, memberChanges);
    RefreshMembers(lobby);
}

// One lobby a lobby.found answer lists.
struct Listing
{
    std::string lobbyId, connection, owner;
    long long maxMembers, members, locked;
    std::map<std::string, std::string> properties;
};

Listing ReadListing(hellforge::FrameReader& reader)
{
    Listing listing;
    listing.lobbyId = reader.Text();
    listing.connection = reader.Text();
    listing.owner = reader.Text();
    listing.maxMembers = reader.Int();
    listing.members = reader.Int();
    listing.locked = reader.Int();
    listing.properties = reader.Properties();
    return listing;
}

// A listing the backend should never pass on: more search properties than PlayFab allows, an impossible member count.
bool Unlisted(const Listing& listing)
{
    return listing.properties.size() > PFLobbyMaxSearchPropertyCount || listing.maxMembers < (long long)PFLobbyMaxMemberCountLowerLimit ||
        listing.maxMembers > (long long)PFLobbyMaxMemberCountUpperLimit || listing.members < 0 || listing.members > listing.maxMembers ||
        listing.lobbyId.empty() || listing.connection.empty();
}

// A listing as one of the search's results; the search keeps the text its fields point to.
void AddResult(SearchResults& search, const Listing& listing)
{
    PFLobbySearchResult entry{};
    entry.lobbyId = search.Keep(listing.lobbyId);
    entry.connectionString = search.Keep(listing.connection);
    search.owners.push_back(PFEntityKey{search.Keep(listing.owner), search.Keep("title_player_account")});
    entry.ownerEntity = &search.owners.back();
    entry.maxMemberCount = (uint32_t)listing.maxMembers; /* both in range: checked by Unlisted */
    entry.currentMemberCount = (uint32_t)listing.members;
    entry.membershipLock = listing.locked != 0 ? PFLobbyMembershipLock::Locked : PFLobbyMembershipLock::Unlocked;
    search.keys.emplace_back();
    search.values.emplace_back();
    for (const auto& property : listing.properties)
    {
        search.keys.back().push_back(search.Keep(property.first));
        search.values.back().push_back(search.Keep(property.second));
    }
    entry.searchPropertyCount = (uint32_t)listing.properties.size();
    entry.searchPropertyKeys = search.keys.back().empty() ? noKeys : search.keys.back().data();
    entry.searchPropertyValues = search.values.back().empty() ? noKeys : search.values.back().data();
    search.results.push_back(entry);
}

// lobby.found [op, hresult, count, (lobby id, connection string, owner entity id, max members, members, locked 0|1, search
// properties)...]: the search's completion, with its results.
void Found(const Operation& operation, hellforge::FrameReader& reader)
{
    HRESULT result = (HRESULT)(uint32_t)reader.Int();
    long long count = reader.Int();
    auto search = std::make_shared<SearchResults>();
    for (long long i = 0; reader.ok && SUCCEEDED(result) && count >= 0 && count <= (long long)PFLobbyClientRequestedSearchResultCountUpperLimit && i < count; i++)
    {
        Listing listing = ReadListing(reader);
        if (!reader.ok) break;
        // A listing the backend should never pass on is skipped alone: the frame is still read past it, so the listings
        // after it stay.
        if (Unlisted(listing))
        {
            hellforge::Note("lobby link: skipped a search result with %zu search properties, %lld of %lld members", listing.properties.size(),
                listing.members, listing.maxMembers);
            continue;
        }
        AddResult(*search, listing);
    }
    if (!reader.ok) result = E_FAIL;
    StateChange* change = Enqueue(PFLobbyStateChangeType::FindLobbiesCompleted);
    PFLobbyFindLobbiesCompletedStateChange& completed = change->payload.findLobbies;
    completed.result = result;
    completed.searchingEntity = InternKey(operation.entityId, operation.entityType);
    completed.asyncContext = operation.asyncContext;
    if (SUCCEEDED(result) && !search->results.empty())
    {
        completed.searchResultCount = (uint32_t)search->results.size();
        completed.searchResults = search->results.data();
    }
    change->search = search;
    recentSearches.push_back(search);
    while (recentSearches.size() > 4) recentSearches.pop_front();
    hellforge::Note("lobby link: search found %u lobby(s) (0x%08x)", completed.searchResultCount, (unsigned)result);
}

// lobby.result [op, kind, hresult, snapshot of a created or joined lobby]: a request's answer completes its operation.
void OnResult(hellforge::FrameReader& reader)
{
    std::string id = reader.Text();
    std::string kind = reader.Text();
    HRESULT result = (HRESULT)(uint32_t)reader.Int();
    auto found = waiting.find(id);
    if (!reader.ok || found == waiting.end()) return;
    Operation operation = std::move(found->second);
    waiting.erase(found);
    hellforge::Note("lobby link: %s %s -> 0x%08x", kind.c_str(), id.c_str(), (unsigned)result);
    bool snapshot = SUCCEEDED(result) && (operation.type == OperationType::CreateAndJoin || operation.type == OperationType::Join);
    std::string localId = operation.type == OperationType::CreateAndJoin ? operation.member->id : operation.entityId;
    if (snapshot && !ApplySnapshot(operation.lobby, reader, localId)) result = E_FAIL;
    Finish(operation, result);
}

void OnFound(hellforge::FrameReader& reader)
{
    std::string id = reader.Text();
    auto found = waiting.find(id);
    if (!reader.ok || found == waiting.end() || found->second.type != OperationType::FindLobbies) return;
    Operation operation = std::move(found->second);
    waiting.erase(found);
    Found(operation, reader);
}

// A friend's invitation (the backend took it from a Steam friend only): every entity listening hears it.
void OnInviteReceived(hellforge::FrameReader& reader)
{
    std::string invitedTo = reader.Text(), connection = reader.Text(), inviter = reader.Text();
    if (!reader.ok || invitedTo.empty() || connection.compare(0, sizeof(connectionPrefix) - 1, connectionPrefix) != 0) return;
    hellforge::Note("lobby link: invited to lobby %s by %s (%zu listening)", invitedTo.c_str(), inviter.c_str(), listeningEntities.size());
    for (const std::string& listener : listeningEntities)
    {
        PFLobbyInviteReceivedStateChange& received = Enqueue(PFLobbyStateChangeType::InviteReceived)->payload.inviteReceived;
        received.listeningEntity = InternKey(listener, listeningTypes[listener]);
        received.invitingEntity = InternKey(inviter, "title_player_account");
        received.connectionString = Intern(connection.c_str());
        received.lobbyId = Intern(invitedTo.c_str());
    }
}

void OnMemberAdded(Lobby* lobby, const std::string& lobbyId, hellforge::FrameReader& reader)
{
    std::string memberId = reader.Text(), memberType = reader.Text();
    std::map<std::string, std::string> properties = reader.Properties();
    if (!reader.ok) return;
    for (Member* member : lobby->members)
        if (member->id == memberId) return;
    Member* member = NewMember(lobby, memberId, memberType, false);
    for (const auto& property : properties) member->properties[property.first] = Intern(property.second.c_str());
    RefreshKeys(member->properties, member->keys);
    hellforge::Note("lobby link: member %s joined lobby %s", memberId.c_str(), lobbyId.c_str());
    PFLobbyMemberAddedStateChange& added = Enqueue(PFLobbyStateChangeType::MemberAdded)->payload.memberAdded;
    added.lobby = Handle(lobby);
    added.member = member->Key();
}

void OnMemberRemoved(Lobby* lobby, const std::string& lobbyId, hellforge::FrameReader& reader)
{
    std::string memberId = reader.Text();
    long long reason = reader.Int();
    if (!reader.ok) return;
    for (size_t i = 0; i < lobby->members.size(); i++)
    {
        Member* member = lobby->members[i];
        if (member->id != memberId) continue;
        lobby->members.erase(lobby->members.begin() + (ptrdiff_t)i);
        RefreshMembers(lobby);
        hellforge::Note(reason == (long long)PFLobbyMemberRemovedReason::LocalUserForciblyRemoved ? "lobby link: member %s was removed from lobby %s by its owner"
            : "lobby link: member %s left lobby %s", memberId.c_str(), lobbyId.c_str());
        PFLobbyMemberRemovedStateChange& removed = Enqueue(PFLobbyStateChangeType::MemberRemoved)->payload.memberRemoved;
        removed.lobby = Handle(lobby);
        removed.member = member->Key();
        removed.reason = (PFLobbyMemberRemovedReason)reason;
        break;
    }
}

// An event about a lobby this machine is in: lobby.memberAdded, lobby.memberRemoved, lobby.updated, lobby.disconnected.
void ApplyLobbyEvent(const hellforge::Frame& frame, hellforge::FrameReader& reader)
{
    std::string lobbyId = reader.Text();
    Lobby* lobby = FindById(lobbyId);
    if (!reader.ok || lobby == nullptr) return;
    if (frame.type == "lobby.memberAdded") OnMemberAdded(lobby, lobbyId, reader);
    else if (frame.type == "lobby.memberRemoved") OnMemberRemoved(lobby, lobbyId, reader);
    else if (frame.type == "lobby.updated") ApplyUpdate(lobby, reader);
    else if (frame.type == "lobby.disconnected")
    {
        long long reason = reader.Int();
        hellforge::Note("lobby link: lobby %s closed (reason %lld)", lobbyId.c_str(), reason);
        Disconnect(lobby, (PFLobbyDisconnectingReason)reason);
    }
}

void ApplyRemote(const hellforge::Frame& frame)
{
    hellforge::FrameReader reader(frame);
    if (frame.type == "lobby.result") OnResult(reader);
    else if (frame.type == "lobby.found") OnFound(reader);
    else if (frame.type == "lobby.inviteReceived") OnInviteReceived(reader);
    else ApplyLobbyEvent(frame, reader);
}

// Frames from the backend become state changes. If the backend went away, every waiting request fails and every lobby
// is disconnected, once.
void ProcessRemote()
{
    for (const hellforge::Frame& frame : link.Drain()) ApplyRemote(frame);
    if (!link.Broken() || remoteLost) return;
    remoteLost = true;
    hellforge::Note("lobby link: the backend went away");
    std::map<std::string, Operation> failed;
    failed.swap(waiting);
    for (auto& entry : failed) Finish(entry.second, lobbyNotFound);
    for (Lobby* lobby : lobbies) Disconnect(lobby, PFLobbyDisconnectingReason::ConnectionInterruption);
}

}
