using System.Text.Json;
using System.Text.Json.Nodes;
using HellForge.Core;

namespace HellForge.LocalBackend;

// The planet modifiers this backend serves (HellForge.Core PlanetModifiers, docs/planet-modifiers.md): the
// installation's planet-modifiers.json (backend --planet-modifiers), read again when it changes, or, while this machine
// is joined to a HellForge host, the host's (Multiplayer.PlanetShare), until the host is gone. They change the war's
// Status and effect definitions as served, whichever war it is (after the war script's additions); the catalogue's
// effect definitions are served with every war.
public sealed class PlanetOverrides(string? path, Action<string> log)
{
    readonly object gate = new();
    DateTime fileTime;
    long fileLength = -1;
    PlanetPlan own = PlanetPlan.Empty;
    string ownJson = "";
    long ownVersion;
    PlanetPlan? host;
    string? hostName;

    // This machine's own file as last read (its canonical JSON) and its version, raised when it changes: what a host sends.
    public (string Json, long Version) Own
    {
        get
        {
            Read();
            lock (gate) return (ownJson, ownVersion);
        }
    }

    // What is served now: the host's while joined to one that sends them, else this machine's own.
    public PlanetPlan Current
    {
        get
        {
            Read();
            lock (gate) return host ?? own;
        }
    }

    // The host's planet modifiers (its file's JSON), from the host of the lobby this machine joined.
    public void SetHost(string name, string json)
    {
        PlanetPlan plan;
        try { plan = PlanetPlan.Parse(json, out var problems); foreach (var p in problems.Take(5)) log("Host's planet modifiers: " + p); }
        catch (JsonException error) { log("The host's planet modifiers could not be read: " + error.Message); return; }
        lock (gate) (host, hostName) = (plan, name);
        log($"Playing with {name}'s planet modifiers ({Count(plan)}).");
    }

    // The host is gone: this machine's own come back.
    public void ClearHost()
    {
        string? name;
        lock (gate)
        {
            if (host == null) return;
            (name, host, hostName) = (hostName, null, null);
        }
        log($"{name} is gone: this machine's own planet modifiers come back.");
    }

    public JsonElement Apply(string path, JsonElement body, JsonElement? warInfo)
    {
        if (path == "/WarSeason/GalacticWarEffects" && body.ValueKind == JsonValueKind.Array)
            return Element(PlanetPlan.Effects(JsonNode.Parse(body.GetRawText())!));
        if (path != $"/WarSeason/{LiveWar.WarId}/Status" || body.ValueKind != JsonValueKind.Object) return body;
        var plan = Current;
        if (plan.Planets.Count == 0) return body;
        var problems = new List<string>();
        var served = Element(plan.Status(JsonNode.Parse(body.GetRawText())!, warInfo, problems.Add));
        Report(problems);
        return served;
    }

    static JsonElement Element(JsonNode node)
    {
        using var document = JsonDocument.Parse(node.ToJsonString());
        return document.RootElement.Clone();
    }

    // The same problem is logged once until the file changes.
    readonly HashSet<string> reported = [];
    void Report(List<string> problems)
    {
        lock (gate)
            foreach (var p in problems)
                if (reported.Add(p)) log(p);
    }

    static string Count(PlanetPlan plan) => plan.Planets.Count == 1 ? "1 planet" : $"{plan.Planets.Count} planets";

    // The file, read again when its time or size changed; one that does not parse keeps the last good one.
    void Read()
    {
        if (path == null) return;
        var info = new FileInfo(path);
        var (time, length) = info.Exists ? (info.LastWriteTimeUtc, info.Length) : (default, -1L);
        lock (gate)
        {
            if (time == fileTime && length == fileLength) return;
            (fileTime, fileLength) = (time, length);
            reported.Clear();
            try
            {
                var plan = PlanetPlan.Read(path, out var problems);
                foreach (var p in problems) log("planet-modifiers.json: " + p);
                var json = plan.Planets.Count == 0 ? "" : plan.ToJson();
                if (json != ownJson) ownVersion++;
                (own, ownJson) = (plan, json);
                log(info.Exists ? $"Planet modifiers read: {Count(plan)}." : "No planet modifiers.");
            }
            catch (JsonException error) { log("planet-modifiers.json not read (" + error.Message + "); the last good one stays."); }
            catch (Exception error) when (error is IOException or UnauthorizedAccessException)
            {
                fileLength = -2; /* being written: again on the next request */
                log("planet-modifiers.json not read yet (" + error.Message + ").");
            }
        }
    }
}
