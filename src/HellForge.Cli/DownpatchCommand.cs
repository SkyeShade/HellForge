using System.Text.Json;
using HellForge.SteamDownload;
using HellForge.Core;

// downpatch-create and downpatch-apply: setup's downpatch files (docs/local-downpatch.md) from a terminal. create makes
// one from Steam's HELLDIVERS 2 and an installation's game copy; apply writes the build HellForge runs into a game folder
// from Steam's HELLDIVERS 2 and such a file. Neither reaches the network: the player moves the file.
static class DownpatchCommand
{
    public static int Run(string command, Dictionary<string, string> options)
    {
        var build = SupportedBuild.Current;
        var steamRoot = options.GetValueOrDefault("--steam-root") ?? SteamLocator.SteamRoot();
        var game = (steamRoot == null ? null : SteamLocator.FindGame(steamRoot))
            ?? throw new InvalidDataException("HELLDIVERS 2 is not installed in Steam: a downpatch file is made from and applied to Steam's game.");
        var source = DownpatchSource.Of(game);
        using var cancel = new CancellationTokenSource();
        Console.CancelKeyPress += (_, e) => { e.Cancel = true; cancel.Cancel(); };
        var reported = "";
        void Progress(DownpatchProgress p)
        {
            var line = $"{p.Stage}: {(p.Total > 0 ? p.Done * 100 / p.Total : 100)}%";
            if (line == reported || p.Total > 0 && p.Done * 100 / p.Total % 5 != 0) return;
            reported = line;
            Console.Error.WriteLine(line);
        }
        DownpatchResult result;
        try
        {
            if (command == "downpatch-create")
            {
                var install = Launch.Install(options);
                if (Launch.SessionRunning()) throw new InvalidDataException("A game session is running: stop it first, since it changes the game copy's files.");
                var outFile = options.GetValueOrDefault("--out") ?? Path.Combine(Environment.CurrentDirectory, Downpatch.FileName(source.Pin.BuildId, build.BuildId));
                if (Directory.Exists(outFile)) outFile = Path.Combine(outFile, Downpatch.FileName(source.Pin.BuildId, build.BuildId));
                Console.Error.WriteLine($"Making a downpatch file from Steam's build {source.Pin.BuildId} to build {build.BuildId}. HellForge doesn't host these; you share it yourself.");
                result = DownpatchMaker.Make(install.Game, source, build, outFile, Progress, cancel.Token, new(Workers(options)));
                Console.WriteLine(JsonSerializer.Serialize(new { result, file = LocalInstall.FullPath(outFile) }, CopyPreparer.JsonOptions));
            }
            else
            {
                var destination = Required(options, "--out");
                if (SteamCmd.InSteamapps(destination)) throw new ArgumentException("--out must be outside Steam's steamapps folders.");
                result = Downpatch.Apply(Required(options, "--file"), source, destination, build, Progress, cancel.Token);
                Console.WriteLine(JsonSerializer.Serialize(new { result, destination = LocalInstall.FullPath(destination) }, CopyPreparer.JsonOptions));
            }
            return 0;
        }
        catch (OperationCanceledException)
        {
            Console.Error.WriteLine("HellForge: stopped.");
            return 1;
        }
    }

    static int? Workers(Dictionary<string, string> options) =>
        options.TryGetValue("--workers", out var text) ? int.TryParse(text, out var n) && n is > 0 and <= 64 ? n
            : throw new ArgumentException("--workers is a number from 1 to 64.") : null;

    static string Required(Dictionary<string, string> options, string key) =>
        options.TryGetValue(key, out var value) && !string.IsNullOrWhiteSpace(value) ? value : throw new ArgumentException($"Missing {key}.");
}
