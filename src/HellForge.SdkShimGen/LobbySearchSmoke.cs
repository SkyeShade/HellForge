using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Text;

namespace HellForge.SdkShimGen;

// The Galactic Map's search on one PC: two real backend processes over loopback UDP sharing a folder lobby directory
// (--directory), and two copies of the lobby stand-in. The host's copy creates a public lobby through the PlayFab C API;
// the friend's copy finds it with PFMultiplayerFindLobbies (the game's filter shape), reads the SDK's search result, and
// joins with the connection string it found. A filter for another planet finds nothing; a lobby the host leaves goes.
// It runs twice: as friends, then with HELLFORGE_TEST_STRANGERS=1 on both backends, where the join passes the directory's
// gate for strangers and the map's record for the host comes from the listing (names kept, other account ids 0).
// Both backends have Play with strangers on (their --mods folder's hellforge-mods.json): off, nothing is listed.
public static unsafe class LobbySearchSmoke
{
    const string HostId = "110000100000001", FriendId = "110000100000002";
    const uint FindLobbiesCompleted = 12;
    const string Filter = "string_key1 eq 'f8c9037f96047f7a' and number_key6 eq 0 and number_key3 eq 261 and number_key7 ge 5 and lobby/memberCount le 3";

    [StructLayout(LayoutKind.Sequential)] struct SearchConfiguration { public nint FriendsFilter; public byte* Filter; public byte* Sort; public uint* Count; }
    [StructLayout(LayoutKind.Sequential)] struct FindCompleted
    {
        public uint Type;
        public int Result;
        public LobbyLinkSmoke.EntityKey Searching;
        public nint Context;
        public uint ResultCount;
        public SearchResult* Results;
    }
    [StructLayout(LayoutKind.Sequential)] struct SearchResult
    {
        public byte* LobbyId, Connection;
        public LobbyLinkSmoke.EntityKey* Owner;
        public uint MaxMembers, Members, PropertyCount;
        public byte** Keys, Values;
        public uint FriendCount;
        public LobbyLinkSmoke.EntityKey* Friends;
        public uint Lock;
    }

    sealed record Found(string LobbyId, string Connection, string Owner, uint Max, uint Members, uint Lock, Dictionary<string, string> Properties);

    public static int Run(string standIns, string backend)
    {
        var root = Directory.CreateDirectory(Path.Combine(Path.GetTempPath(), "hellforge-lobby-search-" + Guid.NewGuid().ToString("N"))).FullName;
        var problems = Mode(standIns, backend, Path.Combine(root, "friends"), strangers: false).Select(p => "friends: " + p)
            .Concat(Mode(standIns, backend, Path.Combine(root, "strangers"), strangers: true).Select(p => "strangers: " + p)).ToList();
        return Report(problems, root);
    }

    // A mods folder with Play with strangers on, as the launcher's switch writes it.
    static string StrangersOn(string folder)
    {
        Directory.CreateDirectory(folder);
        HellForge.Core.ModSettings.SetPlayWithStrangers(folder, true);
        return folder;
    }

    static List<string> Mode(string standIns, string backend, string root, bool strangers)
    {
        Directory.CreateDirectory(root);
        var problems = new List<string>();
        void Expect(bool condition, string description) { if (!condition) problems.Add(description); }
        Environment.SetEnvironmentVariable("HELLFORGE_SHIM_LOG_DIR", Path.Combine(root, "logs"));
        var ports = (LobbyLinkSmoke.FreePort(), LobbyLinkSmoke.FreePort());
        var pipes = ($"hellforge-search-host-{Guid.NewGuid():N}", $"hellforge-search-friend-{Guid.NewGuid():N}");
        var directory = Path.Combine(root, "directory");
        var processes = new List<Process>();
        try
        {
            Dictionary<string, string>? environment = strangers ? new() { ["HELLFORGE_TEST_STRANGERS"] = "1" } : null;
            processes.Add(LobbyLinkSmoke.StartBackend(backend, Path.Combine(root, "host"), pipes.Item1, $"udp:{HostId}:{ports.Item1}:{FriendId}={ports.Item2}",
                ["--directory", directory, "--mods", StrangersOn(Path.Combine(root, "host-mods"))], environment));
            processes.Add(LobbyLinkSmoke.StartBackend(backend, Path.Combine(root, "friend"), pipes.Item2, $"udp:{FriendId}:{ports.Item2}:{HostId}={ports.Item1}",
                ["--directory", directory, "--mods", StrangersOn(Path.Combine(root, "friend-mods"))], environment));
            AwaitBackends(root, ["host", "friend"], Expect);
            if (problems.Count > 0) return problems;
            var host = LobbyLinkSmoke.Load(Path.Combine(standIns, "PlayFabMultiplayerWin.dll"), Path.Combine(root, "lobby-host.dll"), pipes.Item1, "host");
            var friend = LobbyLinkSmoke.Load(Path.Combine(standIns, "PlayFabMultiplayerWin.dll"), Path.Combine(root, "lobby-friend.dll"), pipes.Item2, "friend");
            var create = (delegate* unmanaged<nint, LobbyLinkSmoke.EntityKey*, LobbyLinkSmoke.CreateConfiguration*, LobbyLinkSmoke.JoinConfiguration*, nint, nint*, int>)
                NativeLibrary.GetExport(host.Library, "PFMultiplayerCreateAndJoinLobby");
            var connection = (delegate* unmanaged<nint, nint*, int>)NativeLibrary.GetExport(host.Library, "PFLobbyGetConnectionString");
            var hostLeave = (delegate* unmanaged<nint, LobbyLinkSmoke.EntityKey*, nint, int>)NativeLibrary.GetExport(host.Library, "PFLobbyLeave");
            var find = (delegate* unmanaged<nint, LobbyLinkSmoke.EntityKey*, SearchConfiguration*, nint, int>)NativeLibrary.GetExport(friend.Library, "PFMultiplayerFindLobbies");
            var join = (delegate* unmanaged<nint, LobbyLinkSmoke.EntityKey*, byte*, LobbyLinkSmoke.JoinConfiguration*, nint, nint*, int>)
                NativeLibrary.GetExport(friend.Library, "PFMultiplayerJoinLobby");

            fixed (byte* hostId = Encoding.ASCII.GetBytes(HostId + "\0"), friendId = Encoding.ASCII.GetBytes(FriendId + "\0"), type = "title_player_account\0"u8,
                k1 = "1\0"u8, v1 = "f8c9037f96047f7a\0"u8, s1 = "string_key1\0"u8, s6 = "number_key6\0"u8, s3 = "number_key3\0"u8, s7 = "number_key7\0"u8,
                zero = "0\0"u8, planet = "261\0"u8, window = "42\0"u8)
            {
                var hostKey = new LobbyLinkSmoke.EntityKey { Id = hostId, Type = type };
                var friendKey = new LobbyLinkSmoke.EntityKey { Id = friendId, Type = type };
                var lobbyKeys = stackalloc byte*[] { k1 };
                var lobbyValues = stackalloc byte*[] { v1 };
                var searchKeys = stackalloc byte*[] { s1, s6, s3, s7 };
                var searchValues = stackalloc byte*[] { v1, zero, planet, window };
                var configuration = new LobbyLinkSmoke.CreateConfiguration
                {
                    MaxMemberCount = 4, LobbyPropertyCount = 1, LobbyKeys = lobbyKeys, LobbyValues = lobbyValues, SearchPropertyCount = 4, SearchKeys = searchKeys,
                    SearchValues = searchValues
                };
                var noMembers = new LobbyLinkSmoke.JoinConfiguration { Count = 0 };
                nint hostLobby = 0, friendLobby = 0, text = 0;
                Expect(create(host.Player.Handle, &hostKey, &configuration, &noMembers, 0x11, &hostLobby) == 0 && host.Player.WaitFor("0:0"), "The host creates a public lobby");
                Expect(connection(hostLobby, &text) == 0, "The host reads its connection string");
                var connectionString = Marshal.PtrToStringUTF8(text) ?? "";

                // The friend's map search finds it once the host's backend lists it (about a second).
                var results = FindHostLobby(friend.Player, find, &friendKey);
                Expect(results.Count == 1, $"The friend's search finds the host's lobby ({results.Count} found)");
                CheckSearchResult(results, connectionString, Expect);
                Expect(Search(friend.Player, find, &friendKey, Filter.Replace("eq 261", "eq 100")).Count == 0, "A search for another planet finds nothing");
                Records(root, friend.Player, find, &friendKey, connectionString, strangers, Expect);

                // The friend joins with the connection string its search found.
                fixed (byte* connectionBytes = Encoding.UTF8.GetBytes((results.FirstOrDefault()?.Connection ?? connectionString) + "\0"))
                    Expect(join(friend.Player.Handle, &friendKey, connectionBytes, &noMembers, 0x22, &friendLobby) == 0 && friend.Player.WaitFor("1:0"),
                        "The friend joins the lobby it found");
                Expect(host.Player.WaitFor("2"), "The host sees the friend join");

                // The host leaves: its listing goes and the search finds nothing.
                Expect(hostLeave(hostLobby, &hostKey, 0x33) == 0 && host.Player.WaitFor("10"), "The host leaves its lobby");
                Expect(WaitGone(friend.Player, find, &friendKey), "A lobby its host left is no longer found");
            }
            return problems;
        }
        finally
        {
            foreach (var process in processes)
            {
                try { if (!process.HasExited) process.Kill(true); } catch (InvalidOperationException) { }
                process.Dispose();
            }
        }
    }

    // The map's record for the host (POST /lobby/accounts on the friend's backend), once the host's game uploaded its lobby.
    // Between strangers it comes from the listing, so only the host's account id is left; friends get it from presence.
    static void Records(string root, LobbyLinkSmoke.Player friend, delegate* unmanaged<nint, LobbyLinkSmoke.EntityKey*, SearchConfiguration*, nint, int> find,
        LobbyLinkSmoke.EntityKey* friendKey, string connectionString, bool strangers, Action<bool, string> expect)
    {
        string Url(string name)
        {
            using var ready = System.Text.Json.JsonDocument.Parse(File.ReadAllText(Path.Combine(root, name, "ready.json")));
            return ready.RootElement.GetProperty("urls")[0].GetString()!;
        }
        using var client = new HttpClient(new HttpClientHandler { UseProxy = false }) { Timeout = TimeSpan.FromSeconds(10) };
        var host = Convert.ToUInt64(HostId, 16).ToString(System.Globalization.CultureInfo.InvariantCulture);
        var lobbyId = connectionString.Split(';')[1];
        var record = "{\"players\":[{\"playerName\":\"Host Diver\",\"memberAccountId\":\"" + host + "\",\"isHost\":true}," +
            "{\"playerName\":\"Second\",\"memberAccountId\":\"76561198000000099\",\"isHost\":false}]," +
            "\"memberAccountIds\":[\"" + host + "\",\"76561198000000099\"],\"numLobbyMembers\":2,\"regionIndex\":3}";
        using (var put = client.PutAsync(Url("host") + "/lobby", new StringContent(record, Encoding.UTF8, "application/json")).GetAwaiter().GetResult())
            expect(put.IsSuccessStatusCode, "The host's game publishes its lobby");
        using (var member = client.PostAsync(Url("host") + "/lobby/" + lobbyId + "/member", new StringContent("")).GetAwaiter().GetResult())
            expect(member.IsSuccessStatusCode, "The host's game says which lobby it is in");
        System.Text.Json.JsonElement found = default;
        var clock = Stopwatch.StartNew();
        // the listing takes the record at its next write (at most 5 s); a search then remembers it for the map
        while (found.ValueKind != System.Text.Json.JsonValueKind.Object && clock.ElapsedMilliseconds < 20000)
        {
            Search(friend, find, friendKey, Filter);
            using var answer = client.PostAsync(Url("friend") + "/lobby/accounts", new StringContent("{\"ids\":[\"" + host + "\"]}", Encoding.UTF8, "application/json"))
                .GetAwaiter().GetResult();
            using var json = System.Text.Json.JsonDocument.Parse(answer.Content.ReadAsStringAsync().GetAwaiter().GetResult());
            if (json.RootElement.GetProperty("lobbies").GetArrayLength() == 1) found = json.RootElement.GetProperty("lobbies")[0].Clone();
            else Thread.Sleep(500);
        }
        expect(found.ValueKind == System.Text.Json.JsonValueKind.Object && found.GetProperty("lobbyId").GetString() == lobbyId &&
            found.GetProperty("connectionString").GetString() == connectionString && found.GetProperty("players")[0].GetProperty("playerName").GetString() == "Host Diver",
            "The friend's backend answers the map's record for the host");
        if (!strangers || found.ValueKind != System.Text.Json.JsonValueKind.Object) return;
        expect(found.GetProperty("memberAccountIds")[0].GetString() == host && found.GetProperty("memberAccountIds")[1].GetString() == "0" &&
            found.GetProperty("players")[1].GetProperty("memberAccountId").GetString() == "0" && found.GetProperty("players")[1].GetProperty("playerName").GetString() == "Second",
            "Between strangers the record is the listing's: names kept, only the host's account id");
        // test mode is on, and the two backends swapped no presence
        expect(Log(root, "host").Contains("Test mode (HELLFORGE_TEST_STRANGERS)", StringComparison.Ordinal) &&
            !Log(root, "friend").Contains("Profile of", StringComparison.Ordinal), "Strangers swap no presence (test mode on)");
    }

    // A backend's multiplayer.log, which it keeps open for writing.
    static string Log(string root, string name)
    {
        using var stream = new FileStream(Path.Combine(root, name, "multiplayer.log"), FileMode.Open, FileAccess.Read, FileShare.ReadWrite);
        using var reader = new StreamReader(stream);
        return reader.ReadToEnd();
    }

    // Waits (up to 20 s each) for each named backend's ready.json.
    static void AwaitBackends(string root, string[] names, Action<bool, string> Expect)
    {
        foreach (var name in names)
        {
            var ready = Path.Combine(root, name, "ready.json");
            var clock = Stopwatch.StartNew();
            while (!File.Exists(ready) && clock.ElapsedMilliseconds < 20000) Thread.Sleep(50);
            Expect(File.Exists(ready), $"The {name} backend starts");
        }
    }

    // The single search result names the lobby, its host and members, and carries its search properties.
    static void CheckSearchResult(List<Found> results, string connectionString, Action<bool, string> Expect)
    {
        if (results.Count != 1) return;
        var found = results[0];
        Expect(found.Connection == connectionString && found.LobbyId == connectionString.Split(';')[1] && found.Owner == HostId && found.Max == 4 &&
            found.Members == 1 && found.Lock == 0, $"The result names the lobby, its host and its members ({found})");
        Expect(found.Properties.GetValueOrDefault("number_key3") == "261" && found.Properties.GetValueOrDefault("string_key1") == "f8c9037f96047f7a" &&
            found.Properties.Count == 4, "The result carries the lobby's search properties: " + string.Join(", ", found.Properties.Keys));
    }

    // The host's lobby once the directory lists it (about a second), by the friend's map search, or empty after 15 s.
    static List<Found> FindHostLobby(LobbyLinkSmoke.Player friend, delegate* unmanaged<nint, LobbyLinkSmoke.EntityKey*, SearchConfiguration*, nint, int> find,
        LobbyLinkSmoke.EntityKey* friendKey)
    {
        List<Found> results = [];
        var clock = Stopwatch.StartNew();
        while (results.Count == 0 && clock.ElapsedMilliseconds < 15000)
        {
            results = Search(friend, find, friendKey, Filter);
            if (results.Count == 0) Thread.Sleep(500);
        }
        return results;
    }

    // Waits (up to 10 s) until the friend's search no longer finds the host's lobby.
    static bool WaitGone(LobbyLinkSmoke.Player friend, delegate* unmanaged<nint, LobbyLinkSmoke.EntityKey*, SearchConfiguration*, nint, int> find,
        LobbyLinkSmoke.EntityKey* friendKey)
    {
        var clock = Stopwatch.StartNew();
        var gone = false;
        while (!gone && clock.ElapsedMilliseconds < 10000)
        {
            gone = Search(friend, find, friendKey, Filter).Count == 0;
            if (!gone) Thread.Sleep(500);
        }
        return gone;
    }

    // One search through the stand-in: its FindLobbiesCompleted's results, read before the batch is finished.
    static List<Found> Search(LobbyLinkSmoke.Player player, delegate* unmanaged<nint, LobbyLinkSmoke.EntityKey*, SearchConfiguration*, nint, int> find,
        LobbyLinkSmoke.EntityKey* searcher, string filter)
    {
        var results = new List<Found>();
        var context = (nint)Random.Shared.Next(0x1000, 0x7FFFFFFF);
        fixed (byte* filterBytes = Encoding.UTF8.GetBytes(filter + "\0"))
        {
            var count = 50u;
            var configuration = new SearchConfiguration { Filter = filterBytes, Count = &count };
            if (find(player.Handle, searcher, &configuration, context) != 0) return results;
        }
        var clock = Stopwatch.StartNew();
        while (clock.ElapsedMilliseconds < 5000)
        {
            uint changeCount;
            nint changes;
            if (player.Start(player.Handle, &changeCount, &changes) != 0) return results;
            var done = false;
            for (var i = 0; i < changeCount; i++)
            {
                var change = (FindCompleted*)((nint*)changes)[i];
                if (change->Type != FindLobbiesCompleted || change->Context != context) continue;
                done = true;
                CollectResults(change, results);
            }
            player.Finish(player.Handle, changeCount, changes);
            if (done) return results;
            Thread.Sleep(10);
        }
        return results;
    }

    // A completed search's results (each lobby's id, connection, owner, counts and search properties) into results.
    static void CollectResults(FindCompleted* change, List<Found> results)
    {
        for (var r = 0; change->Result == 0 && r < change->ResultCount; r++)
        {
            var result = change->Results[r];
            var properties = new Dictionary<string, string>(StringComparer.Ordinal);
            for (var p = 0; p < result.PropertyCount; p++)
                properties[Marshal.PtrToStringUTF8((nint)result.Keys[p]) ?? ""] = Marshal.PtrToStringUTF8((nint)result.Values[p]) ?? "";
            results.Add(new Found(Marshal.PtrToStringUTF8((nint)result.LobbyId) ?? "", Marshal.PtrToStringUTF8((nint)result.Connection) ?? "",
                result.Owner != null ? Marshal.PtrToStringUTF8((nint)result.Owner->Id) ?? "" : "", result.MaxMembers, result.Members, result.Lock, properties));
        }
    }

    static int Report(List<string> problems, string root)
    {
        Console.WriteLine($"Lobby search smoke (two backends, a folder directory, two stand-ins): {(problems.Count == 0 ? "passed" : problems.Count + " problem(s)")}; logs in {root}");
        foreach (var problem in problems) Console.Error.WriteLine("  failed: " + problem);
        return problems.Count == 0 ? 0 : 1;
    }
}
