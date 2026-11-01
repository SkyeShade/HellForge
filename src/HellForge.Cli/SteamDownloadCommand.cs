using System.Diagnostics;
using System.Text.Json;
using HellForge.Core;
using HellForge.SteamDownload;

// steam-download: setup's download of the supported build from Valve's servers (docs/steam-download.md), from a terminal.
// Signs in to Steam with a QR code to scan with the Steam app, plans the build's files against what Steam's install already
// holds, and writes them into --out (or, with --dry-run yes, prints the plan only). --file takes one file of the build, for
// a quick check. --sign-in steamcmd lets Valve's SteamCMD download it instead (no phone needed): SteamCMD asks for the
// password in this terminal, itself, and HellForge never reads it; --steamcmd is its folder (default: beside --out).
// Nothing of the sign-in is kept or printed.
static class SteamDownloadCommand
{
    public static int Run(Dictionary<string, string> options)
    {
        var build = SupportedBuild.Current;
        var destination = LocalInstall.FullPath(options.TryGetValue("--out", out var output) && output.Length > 0 ? output
            : throw new ArgumentException("Missing --out."));
        var steamRoot = options.GetValueOrDefault("--steam-root") ?? SteamLocator.SteamRoot();
        var installed = FindInstalled(steamRoot);
        OutsideSteam(destination, steamRoot, "--out");
        var only = options.TryGetValue("--file", out var file) ? file.Replace('\\', '/') : null;
        if (only != null && !build.Files.Any(f => f.Path == only)) throw new ArgumentException($"Not a file of build {build.BuildId}: {only}.");
        var job = new BuildDownloadJob(build, destination, steamRoot, installed, only != null ? [only] : null);
        var dryRun = options.GetValueOrDefault("--dry-run") switch { null or "no" => false, "yes" => true, _ => throw new ArgumentException("--dry-run is yes or no.") };
        var steamCmd = SteamCmdOf(options, destination, steamRoot, dryRun);
        using var cancel = new CancellationTokenSource();
        Console.CancelKeyPress += (_, e) => { e.Cancel = true; cancel.Cancel(); };
        Console.Error.WriteLine($"HellForge gets HELLDIVERS 2 build {build.BuildId} from Steam for your own account. Nothing of the sign-in is kept.");
        try
        {
            var result = steamCmd != null
                ? BySteamCmd(steamCmd, options.GetValueOrDefault("--account"), job, cancel.Token).GetAwaiter().GetResult()
                : Download(job, dryRun, cancel.Token).GetAwaiter().GetResult();
            Console.WriteLine(JsonSerializer.Serialize(result, CopyPreparer.JsonOptions));
            return 0;
        }
        catch (Exception error) when (error is SteamSignInException or SteamCmdException or DepotAccessDeniedException or HttpRequestException
            or TimeoutException or OperationCanceledException)
        {
            Console.Error.WriteLine("HellForge: " + (error is OperationCanceledException ? "stopped." : error.Message));
            return 1;
        }
    }

    // --sign-in steamcmd: SteamCMD's folder (outside Steam's libraries, like --out); null for the QR code sign-in.
    static SteamCmd? SteamCmdOf(Dictionary<string, string> options, string destination, string? steamRoot, bool dryRun)
    {
        switch (options.GetValueOrDefault("--sign-in"))
        {
            case null or "qr": return null;
            case "steamcmd": break;
            default: throw new ArgumentException("--sign-in is qr or steamcmd.");
        }
        if (dryRun) throw new ArgumentException("--dry-run is for the QR code sign-in, which plans before it downloads.");
        var folder = options.GetValueOrDefault("--steamcmd") is { Length: > 0 } given ? given : destination + "-steamcmd";
        OutsideSteam(folder, steamRoot, "--steamcmd");
        var steamCmd = new SteamCmd(folder);
        return steamCmd;
    }

    // SteamCMD downloads the build's depots (only --file's with --file) into its folder; each file is checked and moved
    // into --out, and SteamCMD is removed. Its remembered sign-in is removed after every run.
    static async Task<object> BySteamCmd(SteamCmd steamCmd, string? account, BuildDownloadJob job, CancellationToken cancellation)
    {
        account ??= Ask("Steam account name (the name you sign in with; SteamCMD asks for the password itself): ");
        if (!SteamCmd.IsAccountName(account)) throw new ArgumentException("A Steam account name has only letters, digits and underscores.");
        var files = job.Only?.ToHashSet();
        var depots = job.Build.Files.Where(f => files == null || files.Contains(f.Path)).Select(f => f.Depot).ToHashSet();
        Console.Error.WriteLine($"Getting SteamCMD from Valve into {steamCmd.Folder}...");
        using (var http = new HttpClient { Timeout = TimeSpan.FromMinutes(5) }) await steamCmd.InstallAsync(http, cancellation);
        Console.Error.WriteLine("SteamCMD updates itself...");
        await steamCmd.UpdateAsync(line => Console.Error.WriteLine("  " + line), cancellation);
        var clock = Stopwatch.StartNew();
        int exitCode;
        try
        {
            steamCmd.ForgetSignIn();
            Console.Error.WriteLine("SteamCMD signs in now. Type your password, and a Steam Guard code if Steam asks, at SteamCMD's prompt " +
                "below: it goes to Valve's own tool, not HellForge.");
            using var process = steamCmd.Start(SteamCmd.Arguments(account, job.Build, depots), visible: true);
            exitCode = await steamCmd.WaitAsync(process, cancellation);
        }
        finally
        {
            steamCmd.Stop();
            steamCmd.ForgetSignIn();
        }
        var states = SteamDepotDownload.Status(steamCmd.Folder, job.Build).Where(d => depots.Contains(d.Depot.Id)).ToList();
        var wanted = job.Build.Files.Where(f => files == null || files.Contains(f.Path)).ToList();
        if (!wanted.All(f => new FileInfo(SteamDepotDownload.Locate(steamCmd.Folder, job.Build, f)) is { Exists: true } file && file.Length == f.Size))
            throw new SteamCmdException(SteamCmd.Unfinished(exitCode, states));
        GameCopier.MoveFromDepots(steamCmd.Folder, job.Destination, job.Build, files);
        steamCmd.Remove();
        return new
        {
            job.Build.BuildId, files = wanted.Count, bytes = wanted.Sum(f => f.Size), by = "steamcmd", destination = job.Destination,
            seconds = (int)clock.Elapsed.TotalSeconds
        };
    }

    static async Task<object> Download(BuildDownloadJob job, bool dryRun, CancellationToken cancellation)
    {
        await using var steam = await SteamDepotSession.SignInAsync(new TerminalPrompt(), cancellation);
        Console.Error.WriteLine("Signed in.");
        void Status(string text) => Console.Error.WriteLine(text);
        if (dryRun)
        {
            var planned = await SteamBuildDownload.PlanAsync(steam, job, Status, cancellation);
            return new
            {
                job.Build.BuildId, files = planned.Plan.Files.Count, bytes = planned.Plan.TotalBytes,
                knownLocalBytes = planned.Plan.KnownLocalBytes, downloadAtMostBytes = planned.Plan.DownloadBytes, notes = planned.Notes,
                plan = planned.Plan.Files.Select(f => new { f.File.Path, f.File.Size, chunks = f.Chunks.Count, f.KnownLocalBytes, downloadAtMostBytes = f.DownloadBytes })
            };
        }
        var clock = Stopwatch.StartNew();
        var reported = -1;
        AssembleProgress? last = null;
        void Progress(AssembleProgress p)
        {
            last = p;
            var percent = p.Total > 0 ? (int)(p.Done * 100 / p.Total) : 100;
            if (percent / 5 == reported / 5) return;
            reported = percent;
            Console.Error.WriteLine($"{percent}%: {p.Done >> 20} of {p.Total >> 20} MB ({p.Downloaded >> 20} MB downloaded, {p.Copied >> 20} MB copied from Steam's install)");
        }
        var done = await SteamBuildDownload.DownloadAsync(steam, job, Status, Progress, cancellation);
        return new
        {
            job.Build.BuildId, files = done.Plan.Files.Count, bytes = done.Plan.TotalBytes, downloadedBytes = last?.Downloaded ?? 0,
            copiedBytes = last?.Copied ?? 0, destination = job.Destination, seconds = (int)clock.Elapsed.TotalSeconds, notes = done.Notes
        };
    }

    // Steam's installed HELLDIVERS 2 to copy from, if any (one that cannot be used counts as none: everything downloads).
    static SteamGame? FindInstalled(string? steamRoot)
    {
        if (steamRoot == null) return null;
        try { return SteamLocator.FindGame(steamRoot); }
        catch (Exception error) when (error is InvalidDataException or IOException)
        {
            Console.Error.WriteLine("Steam's own HELLDIVERS 2 cannot be read (" + error.Message + "); every file is downloaded.");
            return null;
        }
    }

    // The download never writes into a Steam library.
    // A path in any steamapps folder is refused even when Steam's own folder is not known (Linux, where SteamRoot finds
    // none), and so is one in a library of a Steam folder that is.
    static void OutsideSteam(string path, string? steamRoot, string option)
    {
        if (SteamCmd.InSteamapps(path) || steamRoot != null && LocalInstall.FindLibraries(steamRoot)
                .Any(library => LocalInstall.Within(LocalInstall.FullPath(path), Path.Combine(library, "steamapps"))))
            throw new ArgumentException($"{option} must be outside Steam's steamapps folders.");
    }

    static string Ask(string question)
    {
        Console.Error.Write(question);
        return Console.ReadLine()?.Trim() is { Length: > 0 } answer ? answer : throw new ArgumentException("No answer given.");
    }

    sealed class TerminalPrompt : ISignInPrompt
    {
        public void ShowQr(string challenge)
        {
            Console.Error.WriteLine();
            Console.Error.Write(QrCode.Terminal(challenge));
            Console.Error.WriteLine("Open the Steam app on your phone, tap the Steam Guard shield, then the QR scanner, and scan this code.");
        }

        public void Status(string text) => Console.Error.WriteLine(text);
    }
}
