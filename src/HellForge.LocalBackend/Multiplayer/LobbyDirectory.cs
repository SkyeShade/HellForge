using System.Globalization;
using System.IO.Compression;
using System.Text;
using System.Text.Json.Nodes;

namespace HellForge.LocalBackend.Multiplayer;

// The lobby directory (docs/multiplayer.md step 7): every backend lists the public lobby it hosts,
// and a searching backend answers the game's Galactic Map search (PFMultiplayerFindLobbies) and the /lobby/accounts that
// follows from the listings it found. On Steam it is one Invisible Steam lobby per listing (SteamLobbyDirectory); tests
// use MemoryLobbyDirectory, the two-backend loopback harness FolderLobbyDirectory.
//
// A listing's data (DirectoryKeys): the protocol, the HellForge connection string, the host's account id, the lobby's
// PlayFab search properties under their own names (string_key1, number_key6, ...), its members, the host's lobby record
// (what /lobby/accounts answers), its live record, its mod set and whether it shares it, and whether its host plays with
// edited stats. Long values are split into parts (DirectoryData).
public sealed record DirectoryListing(ulong Host, IReadOnlyDictionary<string, string> Data);

// What a search asks the directory for: the protocol always; a value a key must equal (the build, privacy), coarse
// filters a directory may apply before the backend evaluates the game's whole filter (LobbyFilter).
public sealed record DirectoryQuery(string Protocol, IReadOnlyDictionary<string, string> Equal, int Count = 50);

public interface ILobbyDirectory
{
    // This machine's listing (null: none). The directory keeps one and replaces it as it changes.
    void Publish(DirectoryListing? listing);
    Task<IReadOnlyList<DirectoryListing>> SearchAsync(DirectoryQuery query, CancellationToken cancellation);
    // Whether the directory holds this machine's last listing now (Steam's lobby may still be being made, or was lost);
    // the in-process and folder directories hold it at once.
    bool Holds => true;
}

public static class DirectoryKeys
{
    public const string Protocol = "hellforge", Connection = "hf.connection", Host = "hf.host", LobbyId = "hf.lobbyId", Members = "hf.members",
        MaxMembers = "hf.maxMembers", Lock = "hf.lock", Record = "hf.lobby", Live = "hf.live", Mods = "hf.mods", Share = "hf.share",
        Digest = "hf.digest", Stats = "hf.stats", Slots = "hf.slots";
}

// Values over PartLength characters are stored as parts: the key holds "#<count>", key.0 .. key.<count-1> the text.
// Steam accepted a 4,000-character value in the probe (8,192 bytes is its documented limit).
public static class DirectoryData
{
    public const int PartLength = 4000, MaxParts = 8;

    public static void Put(IDictionary<string, string> data, string key, string value)
    {
        if (value.Length <= PartLength && !value.StartsWith('#')) { data[key] = value; return; }
        var parts = (value.Length + PartLength - 1) / PartLength;
        if (parts > MaxParts) throw new InvalidDataException($"{key} is too long for the directory ({value.Length} characters).");
        data[key] = "#" + parts.ToString(CultureInfo.InvariantCulture);
        for (var i = 0; i < parts; i++) data[key + "." + i.ToString(CultureInfo.InvariantCulture)] = value.Substring(i * PartLength, Math.Min(PartLength, value.Length - i * PartLength));
    }

    public static string? Get(IReadOnlyDictionary<string, string> data, string key)
    {
        if (!data.TryGetValue(key, out var value)) return null;
        if (!value.StartsWith('#')) return value;
        if (!int.TryParse(value.AsSpan(1), NumberStyles.None, CultureInfo.InvariantCulture, out var parts) || parts is < 1 or > MaxParts) return null;
        var text = new StringBuilder();
        for (var i = 0; i < parts; i++)
            if (data.TryGetValue(key + "." + i.ToString(CultureInfo.InvariantCulture), out var part)) text.Append(part);
            else return null;
        return text.ToString();
    }

    // JSON as gzip in base64: the records and the mod set are JSON of a few KB that compress well.
    public static string Pack(string json)
    {
        using var output = new MemoryStream();
        using (var gzip = new GZipStream(output, CompressionLevel.SmallestSize)) gzip.Write(Encoding.UTF8.GetBytes(json));
        return Convert.ToBase64String(output.ToArray());
    }

    public static string? Unpack(string? packed, int limit = 1 << 20)
    {
        if (string.IsNullOrEmpty(packed)) return null;
        try
        {
            using var input = new GZipStream(new MemoryStream(Convert.FromBase64String(packed)), CompressionMode.Decompress);
            using var output = new MemoryStream();
            var buffer = new byte[16 << 10];
            int read;
            while ((read = input.Read(buffer)) > 0)
            {
                if (output.Length + read > limit) return null;
                output.Write(buffer, 0, read);
            }
            return Encoding.UTF8.GetString(output.ToArray());
        }
        catch (Exception error) when (error is FormatException or InvalidDataException) { return null; }
    }

    // A listing's data as one SHA-256 (lowercase hex), keys in ordinal order, each key and value length-prefixed: what a
    // friend's backend vouches for (FriendPresence.Vouches).
    public static string Digest(IReadOnlyDictionary<string, string> data)
    {
        var text = new StringBuilder();
        foreach (var (key, value) in data.OrderBy(d => d.Key, StringComparer.Ordinal))
            text.Append(key.Length).Append(':').Append(key).Append(value.Length).Append(':').Append(value);
        return Convert.ToHexStringLower(System.Security.Cryptography.SHA256.HashData(Encoding.UTF8.GetBytes(text.ToString())));
    }

    // Does the listing match the query's coarse filters (what a directory may apply itself)?
    public static bool Matches(DirectoryListing listing, DirectoryQuery query) =>
        listing.Data.GetValueOrDefault(DirectoryKeys.Protocol) == query.Protocol && query.Equal.All(e => listing.Data.GetValueOrDefault(e.Key) == e.Value);
}

// Backends in one process (tests).
public sealed class MemoryLobbyDirectory
{
    readonly Dictionary<ulong, DirectoryListing> listings = [];

    public ILobbyDirectory For(ulong host) => new Node(this, host);

    public IReadOnlyCollection<DirectoryListing> All { get { lock (listings) return [.. listings.Values]; } }

    sealed class Node(MemoryLobbyDirectory directory, ulong host) : ILobbyDirectory
    {
        public void Publish(DirectoryListing? listing)
        {
            lock (directory.listings)
                if (listing == null) directory.listings.Remove(host);
                else directory.listings[host] = listing with { Host = host };
        }

        public Task<IReadOnlyList<DirectoryListing>> SearchAsync(DirectoryQuery query, CancellationToken cancellation)
        {
            lock (directory.listings)
                return Task.FromResult<IReadOnlyList<DirectoryListing>>([.. directory.listings.Values.Where(l => DirectoryData.Matches(l, query)).Take(query.Count)]);
        }
    }
}

// Backends on one PC (the loopback UDP harness): a listing is a JSON file per host in a shared folder.
public sealed class FolderLobbyDirectory(string folder, ulong host) : ILobbyDirectory
{
    string File(ulong id) => Path.Combine(folder, id.ToString("X", CultureInfo.InvariantCulture) + ".json");

    public void Publish(DirectoryListing? listing)
    {
        Directory.CreateDirectory(folder);
        if (listing == null) { System.IO.File.Delete(File(host)); return; }
        var data = new JsonObject(listing.Data.OrderBy(d => d.Key, StringComparer.Ordinal).Select(d => KeyValuePair.Create(d.Key, (JsonNode?)d.Value)));
        System.IO.File.WriteAllText(File(host) + ".partial", data.ToJsonString());
        System.IO.File.Move(File(host) + ".partial", File(host), overwrite: true);
    }

    public Task<IReadOnlyList<DirectoryListing>> SearchAsync(DirectoryQuery query, CancellationToken cancellation)
    {
        var found = new List<DirectoryListing>();
        if (Directory.Exists(folder))
            foreach (var path in Directory.GetFiles(folder, "*.json"))
                try
                {
                    if (!ulong.TryParse(Path.GetFileNameWithoutExtension(path), NumberStyles.AllowHexSpecifier, CultureInfo.InvariantCulture, out var id) ||
                        JsonNode.Parse(System.IO.File.ReadAllText(path)) is not JsonObject data) continue;
                    var listing = new DirectoryListing(id, data.Where(d => d.Value is JsonValue).ToDictionary(d => d.Key, d => (string?)d.Value ?? "", StringComparer.Ordinal));
                    if (DirectoryData.Matches(listing, query)) found.Add(listing);
                }
                catch (Exception error) when (error is IOException or System.Text.Json.JsonException or InvalidOperationException) { }
        return Task.FromResult<IReadOnlyList<DirectoryListing>>([.. found.Take(query.Count)]);
    }
}

// The game's search filter (PFLobbySearchConfiguration.filterString): clauses "<key> <eq|ne|gt|lt|ge|le> <value>" joined by
// "and" (the only connective the SDK takes); a value is a number, true/false, or text in single quotes ('' for a quote).
// Keys are the lobby's search properties (string_keyN, number_keyN) or the SDK's lobby/... keys. A clause on a key the
// lobby lacks does not match, as on PlayFab. Two of the Galactic Map's clauses are left out on purpose: number_key7's
// random window (it spreads vanilla's many players over lobbies) and the continent exclusions (string_key6 ne), since
// the directory is small and worldwide.
public sealed class LobbyFilter
{
    public sealed record Clause(string Key, string Op, string Value, bool Text);

    public IReadOnlyList<Clause> Clauses { get; }
    public IReadOnlyList<Clause> Ignored { get; }

    LobbyFilter(List<Clause> clauses, List<Clause> ignored) => (Clauses, Ignored) = (clauses, ignored);

    static readonly HashSet<string> Ops = ["eq", "ne", "gt", "lt", "ge", "le"];

    public static LobbyFilter Parse(string? filter)
    {
        var clauses = new List<Clause>();
        var ignored = new List<Clause>();
        if (string.IsNullOrWhiteSpace(filter)) return new LobbyFilter(clauses, ignored);
        if (filter.Length > 1000) throw new InvalidDataException("The search filter is too long.");
        var tokens = Tokens(filter);
        for (var i = 0; i < tokens.Count;)
        {
            if (i + 2 >= tokens.Count || tokens[i].Text || !Ops.Contains(tokens[i + 1].Value) || tokens[i + 1].Text)
                throw new InvalidDataException("The search filter is malformed: " + filter);
            var clause = new Clause(tokens[i].Value, tokens[i + 1].Value, tokens[i + 2].Value, tokens[i + 2].Text);
            (Dropped(clause) ? ignored : clauses).Add(clause);
            i += 3;
            if (i == tokens.Count) break;
            if (tokens[i].Text || !tokens[i].Value.Equals("and", StringComparison.OrdinalIgnoreCase)) throw new InvalidDataException("The search filter is malformed: " + filter);
            i++;
            if (i == tokens.Count) throw new InvalidDataException("The search filter ends with and: " + filter);
        }
        return new LobbyFilter(clauses, ignored);
    }

    static bool Dropped(Clause clause) => clause.Key == "number_key7" || clause.Key == "string_key6" && clause.Op == "ne";

    // Words and quoted texts.
    static List<(string Value, bool Text)> Tokens(string filter)
    {
        var tokens = new List<(string, bool)>();
        for (var i = 0; i < filter.Length;)
        {
            if (char.IsWhiteSpace(filter[i])) { i++; continue; }
            if (filter[i] == '\'')
            {
                tokens.Add((Quoted(filter, ref i), true));
                continue;
            }
            var start = i;
            while (i < filter.Length && !char.IsWhiteSpace(filter[i]) && filter[i] != '\'') i++;
            tokens.Add((filter[start..i], false));
        }
        return tokens;
    }

    // The quoted text that starts at i ('' a quote inside it); i moves past its closing quote.
    static string Quoted(string filter, ref int i)
    {
        var text = new StringBuilder();
        for (i++; ; i++)
        {
            if (i >= filter.Length) throw new InvalidDataException("The search filter has an open quote: " + filter);
            if (filter[i] != '\'') { text.Append(filter[i]); continue; }
            if (i + 1 < filter.Length && filter[i + 1] == '\'') { text.Append('\''); i++; continue; }
            i++;
            break;
        }
        return text.ToString();
    }

    // The values of every clause that compares a key with eq: what a directory can filter on itself.
    public IReadOnlyDictionary<string, string> Equalities() =>
        Clauses.Where(c => c.Op == "eq" && !c.Key.StartsWith("lobby/", StringComparison.Ordinal)).GroupBy(c => c.Key).Where(g => g.Count() == 1)
            .ToDictionary(g => g.Key, g => g.Single().Value, StringComparer.Ordinal);

    // members: the lobby's member count, max its limit, locked its membership lock; properties its search properties.
    public bool Matches(IReadOnlyDictionary<string, string> properties, int members, int max, bool locked) =>
        Clauses.All(c => Holds(c, c.Key switch
        {
            "lobby/memberCount" => members.ToString(CultureInfo.InvariantCulture),
            "lobby/memberCountRemaining" => (max - members).ToString(CultureInfo.InvariantCulture),
            "lobby/membershipLock" => locked ? "Locked" : "Unlocked",
            "lobby/amMember" or "lobby/amOwner" => "false",
            _ => properties.GetValueOrDefault(c.Key)
        }));

    static bool Holds(Clause clause, string? actual)
    {
        if (actual == null) return false;
        int order;
        if (!clause.Text && double.TryParse(clause.Value, NumberStyles.Float, CultureInfo.InvariantCulture, out var wanted) &&
            double.TryParse(actual, NumberStyles.Float, CultureInfo.InvariantCulture, out var number))
            order = number.CompareTo(wanted);
        else order = string.CompareOrdinal(actual, clause.Value);
        return clause.Op switch
        {
            "eq" => order == 0, "ne" => order != 0, "gt" => order > 0, "lt" => order < 0, "ge" => order >= 0, _ => order <= 0
        };
    }
}
