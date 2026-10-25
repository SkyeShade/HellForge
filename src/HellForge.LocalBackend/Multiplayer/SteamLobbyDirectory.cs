using System.Collections.Concurrent;
using System.Globalization;
using System.Runtime.InteropServices;
using System.Text;

namespace HellForge.LocalBackend.Multiplayer;

// The lobby directory on Steam matchmaking (ISteamMatchmaking009, the game's app 553850). This machine's listing is an
// Invisible Steam lobby the backend holds beside the game's own regular one, its data the listing's; a search is
// RequestLobbyList with the coarse filters (worldwide, at most 50), each result's data read whole. Measured on Steam
// (2026-10-22): both lobbies coexist, a new lobby is searchable after
// about 0.9 s, a search takes about 200 ms, NotEqual and two numerical filters on one key combine, a 4,000-character
// value is kept. Every Steam call runs on the peer link's thread (SteamPeerTransport pumps this); others queue work.
// A failed CreateLobby is tried again (after 5 s, doubling to a minute), and a listing lobby Steam no longer has this
// machine own (Steam's connection dropped, the lobby was lost) is made again: every CheckInterval its owner is read.
// Holds says whether Steam holds the listing now (DirectoryService.Listing).
public sealed unsafe class SteamLobbyDirectory(Action<string> log) : ILobbyDirectory
{
    const int LobbyMatchList = 510, LobbyCreated = 513;
    const int LobbyTypeInvisible = 3, DistanceWorldwide = 3, Equal = 0, GreaterOrEqual = 2;
    static readonly TimeSpan CallTimeout = TimeSpan.FromSeconds(20), SearchTimeout = TimeSpan.FromSeconds(30);
    static readonly TimeSpan FirstRetry = TimeSpan.FromSeconds(5), LastRetry = TimeSpan.FromMinutes(1), CheckInterval = TimeSpan.FromSeconds(10);

    readonly ConcurrentQueue<Action> work = new();
    // Steam calls waiting for their results: call handle -> expected callback, result size, deadline, completion (null: failed).
    readonly Dictionary<ulong, (int Id, int Size, DateTime Deadline, Action<byte[]?> Done)> calls = [];
    readonly Queue<(DirectoryQuery Query, TaskCompletionSource<IReadOnlyList<DirectoryListing>> Done)> searches = new();
    bool searching, bound;
    int pipe;
    nint matchmaking;
    ulong lobby, self;
    bool creating;
    // A failed CreateLobby: when to try again, and the wait after that one; when the lobby's owner was last read.
    DateTime retryAt = DateTime.MaxValue, checkedAt = DateTime.MinValue;
    TimeSpan retryWait = FirstRetry;
    volatile bool holds;
    DirectoryListing? desired;
    readonly Dictionary<string, string> written = new(StringComparer.Ordinal);

    delegate* unmanaged[Cdecl]<int, ulong, void*, int, int, byte*, byte> callResult;
    delegate* unmanaged[Cdecl]<nint, int, int, ulong> createLobby;
    delegate* unmanaged[Cdecl]<nint, ulong, void> leaveLobby;
    delegate* unmanaged[Cdecl]<nint, ulong, ulong> getLobbyOwner;
    delegate* unmanaged[Cdecl]<nint, ulong, byte*, byte*, byte> setLobbyData;
    delegate* unmanaged[Cdecl]<nint, ulong, byte*, byte> deleteLobbyData;
    delegate* unmanaged[Cdecl]<nint, ulong, int> getLobbyDataCount;
    delegate* unmanaged[Cdecl]<nint, ulong, int, byte*, int, byte*, int, byte> getLobbyDataByIndex;
    delegate* unmanaged[Cdecl]<nint, byte*, byte*, int, void> stringFilter;
    delegate* unmanaged[Cdecl]<nint, byte*, int, int, void> numericalFilter;
    delegate* unmanaged[Cdecl]<nint, int, void> distanceFilter;
    delegate* unmanaged[Cdecl]<nint, int, void> resultCountFilter;
    delegate* unmanaged[Cdecl]<nint, ulong> requestLobbyList;
    delegate* unmanaged[Cdecl]<nint, int, ulong> getLobbyByIndex;

    public void Publish(DirectoryListing? listing) => work.Enqueue(() => Apply(listing));

    // Whether Steam holds this machine's listing now: its lobby made, owned by this machine, and the data written.
    public bool Holds => holds;

    public Task<IReadOnlyList<DirectoryListing>> SearchAsync(DirectoryQuery query, CancellationToken cancellation)
    {
        var done = new TaskCompletionSource<IReadOnlyList<DirectoryListing>>(TaskCreationOptions.RunContinuationsAsynchronously);
        work.Enqueue(() => { searches.Enqueue((query, done)); NextSearch(); });
        return done.Task.WaitAsync(SearchTimeout, cancellation);
    }

    // ---- On the Steam thread ----------------------------------------------------------------------------------------

    internal void Bind(Func<string, nint> e, int steamPipe, ulong localId)
    {
        pipe = steamPipe;
        self = localId;
        matchmaking = ((delegate* unmanaged[Cdecl]<nint>)e("SteamAPI_SteamMatchmaking_v009"))();
        if (matchmaking == 0) throw new IOException("ISteamMatchmaking009 is unavailable.");
        callResult = (delegate* unmanaged[Cdecl]<int, ulong, void*, int, int, byte*, byte>)e("SteamAPI_ManualDispatch_GetAPICallResult");
        createLobby = (delegate* unmanaged[Cdecl]<nint, int, int, ulong>)e("SteamAPI_ISteamMatchmaking_CreateLobby");
        leaveLobby = (delegate* unmanaged[Cdecl]<nint, ulong, void>)e("SteamAPI_ISteamMatchmaking_LeaveLobby");
        getLobbyOwner = (delegate* unmanaged[Cdecl]<nint, ulong, ulong>)e("SteamAPI_ISteamMatchmaking_GetLobbyOwner");
        setLobbyData = (delegate* unmanaged[Cdecl]<nint, ulong, byte*, byte*, byte>)e("SteamAPI_ISteamMatchmaking_SetLobbyData");
        deleteLobbyData = (delegate* unmanaged[Cdecl]<nint, ulong, byte*, byte>)e("SteamAPI_ISteamMatchmaking_DeleteLobbyData");
        getLobbyDataCount = (delegate* unmanaged[Cdecl]<nint, ulong, int>)e("SteamAPI_ISteamMatchmaking_GetLobbyDataCount");
        getLobbyDataByIndex = (delegate* unmanaged[Cdecl]<nint, ulong, int, byte*, int, byte*, int, byte>)e("SteamAPI_ISteamMatchmaking_GetLobbyDataByIndex");
        stringFilter = (delegate* unmanaged[Cdecl]<nint, byte*, byte*, int, void>)e("SteamAPI_ISteamMatchmaking_AddRequestLobbyListStringFilter");
        numericalFilter = (delegate* unmanaged[Cdecl]<nint, byte*, int, int, void>)e("SteamAPI_ISteamMatchmaking_AddRequestLobbyListNumericalFilter");
        distanceFilter = (delegate* unmanaged[Cdecl]<nint, int, void>)e("SteamAPI_ISteamMatchmaking_AddRequestLobbyListDistanceFilter");
        resultCountFilter = (delegate* unmanaged[Cdecl]<nint, int, void>)e("SteamAPI_ISteamMatchmaking_AddRequestLobbyListResultCountFilter");
        requestLobbyList = (delegate* unmanaged[Cdecl]<nint, ulong>)e("SteamAPI_ISteamMatchmaking_RequestLobbyList");
        getLobbyByIndex = (delegate* unmanaged[Cdecl]<nint, int, ulong>)e("SteamAPI_ISteamMatchmaking_GetLobbyByIndex");
        bound = true;
    }

    // Each turn of the peer link's loop: queued work, and calls past their deadline fail.
    internal void Pump()
    {
        if (!bound) return;
        while (work.TryDequeue(out var job))
            try { job(); }
            catch (Exception error) when (error is IOException or InvalidDataException) { log("Directory (Steam): " + error.Message); }
        var now = DateTime.UtcNow;
        foreach (var (call, waiting) in calls.Where(c => c.Value.Deadline < now).ToList())
        {
            calls.Remove(call);
            waiting.Done(null);
        }
        if (desired != null && lobby == 0 && !creating && now >= retryAt) { retryAt = DateTime.MaxValue; Apply(desired); }
        if (lobby != 0 && now - checkedAt >= CheckInterval)
        {
            checkedAt = now;
            if (getLobbyOwner(matchmaking, lobby) != self)
            {
                log("Directory (Steam): this machine's listing lobby was lost; listing again.");
                lobby = 0;
                holds = false;
                written.Clear();
                if (desired != null) Apply(desired);
            }
        }
    }

    // SteamAPICallCompleted_t (callback 703): a call of this directory's has its result. Called before the callback is freed.
    internal void CallCompleted(byte* parameter)
    {
        var call = *(ulong*)parameter;
        if (!calls.Remove(call, out var waiting)) return;
        var result = new byte[waiting.Size];
        byte failed = 0;
        fixed (byte* into = result)
            if (callResult(pipe, call, into, waiting.Size, waiting.Id, &failed) == 0 || failed != 0) { waiting.Done(null); return; }
        waiting.Done(result);
    }

    // The link is going: this machine's Steam lobby is left (Steam would on shutdown too).
    internal void Shutdown()
    {
        if (bound && lobby != 0) leaveLobby(matchmaking, lobby);
        lobby = 0;
        holds = false;
        foreach (var (_, done) in searches) done.TrySetException(new IOException("The Steam peer link stopped."));
        searches.Clear();
    }

    void Call(ulong handle, int id, int size, Action<byte[]?> done)
    {
        if (handle == 0) { done(null); return; }
        calls[handle] = (id, size, DateTime.UtcNow + CallTimeout, done);
    }

    void Apply(DirectoryListing? listing)
    {
        desired = listing;
        if (listing == null)
        {
            if (lobby != 0) { leaveLobby(matchmaking, lobby); log("Directory (Steam): left this machine's listing lobby."); }
            lobby = 0;
            holds = false;
            retryAt = DateTime.MaxValue;
            retryWait = FirstRetry;
            written.Clear();
            return;
        }
        if (lobby != 0) { Write(); return; }
        if (creating) return;
        creating = true;
        Call(createLobby(matchmaking, LobbyTypeInvisible, 4), LobbyCreated, 16, result =>
        {
            creating = false;
            // LobbyCreated_t: EResult m_eResult, uint64 m_ulSteamIDLobby (at 8)
            var created = result != null && BitConverter.ToInt32(result, 0) == 1 ? BitConverter.ToUInt64(result, 8) : 0;
            if (created == 0)
            {
                // tried again from Pump, sooner at first; a listing withdrawn meanwhile stops the retries (Apply(null))
                log($"Directory (Steam): CreateLobby failed (EResult {(result != null ? BitConverter.ToInt32(result, 0) : -1)}); trying again in {retryWait.TotalSeconds:0} s.");
                retryAt = DateTime.UtcNow + retryWait;
                retryWait = TimeSpan.FromTicks(Math.Min(retryWait.Ticks * 2, LastRetry.Ticks));
                return;
            }
            retryWait = FirstRetry;
            checkedAt = DateTime.UtcNow;
            lobby = created;
            written.Clear();
            log("Directory (Steam): this machine's listing lobby is " + created.ToString(CultureInfo.InvariantCulture) + ".");
            if (desired == null) Apply(null);
            else Write();
        });
    }

    // The listing's data onto the Steam lobby: changed keys set, keys it no longer has deleted.
    void Write()
    {
        if (desired == null) return;
        var set = 0;
        foreach (var (key, value) in desired.Data)
        {
            if (written.TryGetValue(key, out var old) && old == value) continue;
            fixed (byte* k = Utf8(key), v = Utf8(value))
                if (setLobbyData(matchmaking, lobby, k, v) == 0) { log($"Directory (Steam): SetLobbyData {key} refused."); continue; }
            written[key] = value;
            set++;
        }
        foreach (var key in written.Keys.Where(k => !desired.Data.ContainsKey(k)).ToList())
        {
            fixed (byte* k = Utf8(key)) deleteLobbyData(matchmaking, lobby, k);
            written.Remove(key);
        }
        if (set > 0) log($"Directory (Steam): {set} listing value(s) written.");
        holds = desired.Data.All(d => written.TryGetValue(d.Key, out var value) && value == d.Value);
    }

    // One RequestLobbyList at a time: Steam's filters are global until the request is made.
    void NextSearch()
    {
        if (searching || !searches.TryPeek(out var next)) return;
        searching = true;
        var (query, done) = next;
        Text(DirectoryKeys.Protocol, query.Protocol, Equal);
        foreach (var (key, value) in query.Equal)
            if (key.StartsWith("number_key", StringComparison.Ordinal) && int.TryParse(value, NumberStyles.AllowLeadingSign, CultureInfo.InvariantCulture, out var number))
                fixed (byte* k = Utf8(key)) numericalFilter(matchmaking, k, number, Equal);
            else Text(key, value, Equal);
        fixed (byte* slots = Utf8(DirectoryKeys.Slots)) numericalFilter(matchmaking, slots, 1, GreaterOrEqual);
        distanceFilter(matchmaking, DistanceWorldwide);
        resultCountFilter(matchmaking, Math.Clamp(query.Count, 1, 50));
        Call(requestLobbyList(matchmaking), LobbyMatchList, 4, result =>
        {
            searches.Dequeue();
            searching = false;
            if (result == null) done.TrySetException(new IOException("Steam's lobby search failed."));
            else done.TrySetResult(Read(BitConverter.ToInt32(result, 0)));
            NextSearch();
        });
    }

    // The search's results: each lobby's whole data, its host read from it.
    List<DirectoryListing> Read(int count)
    {
        var found = new List<DirectoryListing>();
        var key = new byte[256];
        var value = new byte[8193];
        for (var i = 0; i < count; i++)
        {
            var id = getLobbyByIndex(matchmaking, i);
            var data = new Dictionary<string, string>(StringComparer.Ordinal);
            var keys = getLobbyDataCount(matchmaking, id);
            for (var j = 0; j < keys && j < 200; j++)
                fixed (byte* k = key, v = value)
                    if (getLobbyDataByIndex(matchmaking, id, j, k, key.Length, v, value.Length) != 0)
                        data[Marshal.PtrToStringUTF8((nint)k) ?? ""] = Marshal.PtrToStringUTF8((nint)v) ?? "";
            if (ulong.TryParse(data.GetValueOrDefault(DirectoryKeys.Host), NumberStyles.None, CultureInfo.InvariantCulture, out var host) && host != 0)
                found.Add(new DirectoryListing(host, data));
        }
        return found;
    }

    void Text(string key, string value, int comparison)
    {
        fixed (byte* k = Utf8(key), v = Utf8(value)) stringFilter(matchmaking, k, v, comparison);
    }

    static byte[] Utf8(string text) => Encoding.UTF8.GetBytes(text + "\0");
}
