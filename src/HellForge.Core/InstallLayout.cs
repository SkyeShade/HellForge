using System.Security.Cryptography;
using System.Text;
using System.Text.Json;

namespace HellForge.Core;

public sealed record InstallMarker(int SchemaVersion, string BuildId, DateTime CreatedUtc);

// A HellForge installation: one folder holding the app, the copied game and everything HellForge and the game write. Nothing
// goes to AppData; outside the folder there are only the Windows firewall rules, which uninstalling removes.
public sealed class InstallLayout(string root)
{
    public const string MarkerFile = "hellforge-install.json";
    // The marker of installations made before the rename (HD2 Beyond The Stars); Find renames it.
    public const string LegacyMarkerFile = "bts-install.json";

    public string Root { get; } = LocalInstall.FullPath(root);
    public string Marker => Path.Combine(Root, MarkerFile);
    public string App => Path.Combine(Root, "app");
    public string Game => Path.Combine(Root, "game");
    // The game's own settings, saves, shader cache, logs and crash dumps; its settings.ini points here.
    public string UserData => Path.Combine(Root, "userdata");
    public string GameProfile(string profile) => Path.Combine(UserData, "profiles", profile);
    // HellForge profiles: each one's backend progress and inventory.
    public string Profiles => Path.Combine(Root, "profiles");
    public string Mods => Path.Combine(Root, "mods");
    public string Logs => Path.Combine(Root, "logs");
    // Mods packed for sharing (ModPackage): outside mods/, where a second copy of a mod would load neither.
    public string Exports => Path.Combine(Root, "exports");
    public string Sessions => Path.Combine(Logs, "sessions");
    public string Temp => Path.Combine(Root, "temp");
    public string VerifyCache => Path.Combine(Temp, "verified-game.json");
    // The build's settings.ini, kept unchanged: each session writes data/settings.ini from it.
    public string OriginalSettings => Path.Combine(Game, "data", "settings.ini" + GameSettings.OriginalSuffix);

    public bool Exists => File.Exists(Marker) || File.Exists(Path.Combine(Root, LegacyMarkerFile));

    // The backend's fixed loopback port, the only one the WFP block lets the copy reach. 40000-46999 is below Windows'
    // dynamic range (49152-65535), where Hyper-V, WSL and Docker reserve port blocks, and clear of Steam's 27000-27100.
    public const int BackendPortFirst = 40000, BackendPortCount = 7000;
    public int ComputedBackendPort
    {
        get
        {
            var hash = SHA256.HashData(Encoding.UTF8.GetBytes(Root.ToLowerInvariant()));
            return BackendPortFirst + (BitConverter.ToUInt16(hash, 16) % BackendPortCount);
        }
    }

    public string NetworkBlockMarker => Path.Combine(Root, "network-block", "wfp-block.json");

    // The port the installed WFP block permits (its marker's "port"); null for a marker from before the block named one,
    // the Linux namespace's marker, or none.
    public int? BlockedBackendPort()
    {
        try
        {
            using var marker = JsonDocument.Parse(File.ReadAllText(NetworkBlockMarker));
            return marker.RootElement.ValueKind == JsonValueKind.Object && marker.RootElement.TryGetProperty("port", out var port) &&
                port.ValueKind == JsonValueKind.Number && port.TryGetInt32(out var value) && value is > 0 and <= 65535 ? value : null;
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException or JsonException) { return null; }
    }

    // What a session starts the backend on: the port the block permits, else (an older block that permits all of loopback,
    // the Linux namespace, the firewall rules) the computed one.
    public int BackendPort => BlockedBackendPort() ?? ComputedBackendPort;

    // The installation containing a folder, such as the launcher's own. An installation from before the rename gets
    // the new marker name.
    public static InstallLayout? Find(string start)
    {
        for (var current = LocalInstall.FullPath(start); current != null; current = Path.GetDirectoryName(current))
        {
            if (File.Exists(Path.Combine(current, MarkerFile))) return new(current);
            var legacy = Path.Combine(current, LegacyMarkerFile);
            if (!File.Exists(legacy)) continue;
            try { File.Move(legacy, Path.Combine(current, MarkerFile)); }
            catch (Exception error) when (error is IOException or UnauthorizedAccessException) { }
            return new(current);
        }
        return null;
    }

    public InstallMarker ReadMarker() =>
        JsonSerializer.Deserialize<InstallMarker>(File.ReadAllText(File.Exists(Marker) ? Marker : Path.Combine(Root, LegacyMarkerFile)),
            CopyPreparer.JsonOptions) ?? throw new InvalidDataException("Empty install marker.");

    // Marks a folder as an installation; refuses a folder that already is one or that lies inside Steam.
    public InstallMarker Create(string buildId)
    {
        LocalInstall.NoLinks(Root);
        if (Exists) throw new InvalidDataException($"{Root} is already a HellForge installation.");
        if (Root.Split(Path.DirectorySeparatorChar).Any(part => part.Equals("steamapps", StringComparison.OrdinalIgnoreCase)))
            throw new InvalidDataException("Install HellForge outside Steam's steamapps folder.");
        foreach (var folder in new[] { Root, App, Game, UserData, Profiles, Logs, Temp }) Directory.CreateDirectory(folder);
        var marker = new InstallMarker(1, buildId, DateTime.UtcNow);
        using var output = new FileStream(Marker, FileMode.CreateNew);
        JsonSerializer.Serialize(output, marker, CopyPreparer.JsonOptions);
        return marker;
    }
}
