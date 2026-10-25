using HellForge.Session;

// HellForge.Session.exe: the engine's session runner (docs/engine.md).
//   play --install <installation> --profile <profile folder> [--template <name>] [--local-playfab] [--multiplayer]
//        [--live-war <cache file> | --open-war | --war-server <https origin> [--war-server-ca <pem file>] [--war-server-token <file>]]
//        [--engine-log] [--game-options <the player's launch options>]
//        [--mod-sync <the mod sync session file>] [--sandbox]   (--sandbox, or HELLFORGE_SANDBOX=1: the game at low integrity
//        with the Steam broker, docs/steam-bridge.md; in development)
//   version
//   census <network-census.jsonl>   (a session's network census, summarised as the session's result.json holds it)
//   network-probe <backend port> <report>   (the network block's probe as a Linux session runs it, PlaySession.NetworkProbeInProcess)
// A session reports through its run folder (logs/sessions/<run>: result.json and the rest, as the launcher reads them);
// anything that stops it before that folder exists goes to logs/session-runner.log.
const string Usage = "Usage: HellForge.Session play --install <installation> --profile <profile folder> [--template <name>] " +
    "[--local-playfab] [--multiplayer] [--live-war <cache file> | --open-war | --war-server <https origin> [--war-server-ca <pem file>] " +
    "[--war-server-token <file>]] [--engine-log] [--game-options <options>] [--mod-sync <file>] [--sandbox] | version | census <log>";

if (args.Length == 1 && args[0] == "version")
{
    Console.WriteLine(typeof(PlaySession).Assembly.GetName().Version);
    return 0;
}
if (args.Length == 2 && args[0] == "census")
{
    Console.WriteLine(NetworkCensus.Summarize(args[1]).ToJsonString(new System.Text.Json.JsonSerializerOptions { WriteIndented = true, Encoder = System.Text.Encodings.Web.JavaScriptEncoder.UnsafeRelaxedJsonEscaping }));
    return 0;
}
if (args.Length == 3 && args[0] == "network-probe" && int.TryParse(args[1], out var probePort))
    return PlaySession.NetworkProbeInProcess(probePort, args[2]);
if (args.Length == 0 || args[0] != "play")
{
    Console.Error.WriteLine(Usage);
    return 2;
}
var options = new PlayOptions();
string? gameOptions = null;
for (var i = 1; i < args.Length; i++)
{
    string Value() => i + 1 < args.Length ? args[++i] : throw new ArgumentException($"{args[i]} needs a value.");
    static bool YesNo(string value, string name) => value switch { "yes" => true, "no" => false, _ => throw new ArgumentException(name + " takes yes or no.") };
    switch (args[i])
    {
        case "--install": options.Install = Value(); break;
        case "--profile": options.Profile = Value(); break;
        case "--template": options.Template = Value(); break;
        case "--live-war": options.LiveWar = Value(); break;
        case "--open-war": options.OpenWar = true; break;
        case "--war-seed": options.WarSeed = uint.Parse(Value(), System.Globalization.NumberStyles.None, System.Globalization.CultureInfo.InvariantCulture); break;
        case "--war-subfactions": options.DealSubfactions = YesNo(Value(), "--war-subfactions"); break;
        case "--war-seaf": options.DealSeaf = YesNo(Value(), "--war-seaf"); break;
        case "--war-server": options.WarServer = Value(); break;
        case "--war-server-ca": options.WarServerCa = Value(); break;
        case "--war-server-token": options.WarServerToken = Value(); break;
        case "--local-playfab": options.LocalPlayFab = true; break;
        case "--multiplayer": options.Multiplayer = true; break;
        case "--engine-log": options.EngineLog = true; break;
        case "--game-options": gameOptions = Value(); break;
        case "--mod-sync": options.ModSync = Value(); break;
        case "--sandbox": options.Sandbox = true; break;
        default: Console.Error.WriteLine(Usage); return 2;
    }
}
if (options.Install.Length == 0) { Console.Error.WriteLine(Usage); return 2; }
// one war: a war server's takes the place of the live and the all-planets war; its CA and token only go with it
if ((options.WarServer != null && (options.LiveWar != null || options.OpenWar)) ||
    (options.WarServer == null && (options.WarServerCa != null || options.WarServerToken != null)))
{ Console.Error.WriteLine(Usage); return 2; }
// as the other HELLFORGE_ diagnostics: a session the launcher starts takes it from the environment
if (Environment.GetEnvironmentVariable("HELLFORGE_SANDBOX") == "1") options.Sandbox = true;
try
{
    // checked as the launcher checks them (GameLaunchOptions): never a switch the session sets itself
    options.GameOptions = HellForge.Core.GameLaunchOptions.Parse(gameOptions);
    if (options.WarServer != null) options.WarServer = HellForge.Core.WarServerOptions.ParseOrigin(options.WarServer);
    return new PlaySession(options).Run();
}
catch (Exception error) when (error is SessionFailure or IOException or UnauthorizedAccessException or ArgumentException or InvalidDataException)
{
    // Before the run folder: the installation's logs folder, else the error stream.
    var logs = Path.Combine(Path.GetFullPath(options.Install), "logs");
    try { Directory.CreateDirectory(logs); File.AppendAllText(Path.Combine(logs, "session-runner.log"), $"{DateTime.Now:yyyy-MM-dd HH:mm:ss} {HellForge.Core.Privacy.Scrub(error.Message)}\n"); }
    catch (IOException) { }
    Console.Error.WriteLine(error.Message);
    return 1;
}

sealed class PlayOptions
{
    public string Install { get; set; } = "";
    public string? Profile { get; set; }
    public string? Template { get; set; }
    public string? LiveWar { get; set; }
    public bool OpenWar { get; set; } /* the all-planets war (backend --war open) */
    public uint WarSeed { get; set; } /* its seed for subfactions and SEAF allies (backend --war-seed) */
    public bool DealSubfactions { get; set; } = true; /* deal its subfactions from the seed (backend --war-subfactions) */
    public bool DealSeaf { get; set; } = true; /* deal its SEAF allies from the seed (backend --war-seaf) */
    public string? WarServer { get; set; } /* a war server's origin (backend --war server), with its CA and token files */
    public string? WarServerCa { get; set; }
    public string? WarServerToken { get; set; }
    public bool LocalPlayFab { get; set; }
    public bool Multiplayer { get; set; }
    public bool EngineLog { get; set; }
    public List<string> GameOptions { get; set; } = [];
    public string? ModSync { get; set; } /* the next session's mods after a mod sync (ModSyncSession); this session only */
    public bool Sandbox { get; set; } /* low-integrity game process with a Steam broker (docs/steam-bridge.md phase 4) */
}
