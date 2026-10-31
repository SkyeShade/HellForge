using System.Diagnostics;
using System.Text.Json;
using HellForge.Core;
using HellForge.SteamDownload;

// steam-download: setup's download of the supported build from Valve's servers (docs/steam-download.md), from a terminal.
// Signs in to Steam (a QR code to scan with the Steam app, or --sign-in password), plans the build's files against what
// Steam's install already holds, and writes them into --out (or, with --dry-run yes, prints the plan only). --file takes
// one file of the build, for a quick check. Nothing of the sign-in is kept or printed.
static class SteamDownloadCommand
{
    public static int Run(Dictionary<string, string> options)
    {
        var build = SupportedBuild.Current;
        var destination = LocalInstall.FullPath(options.TryGetValue("--out", out var output) && output.Length > 0 ? output
            : throw new ArgumentException("Missing --out."));
        var steamRoot = options.GetValueOrDefault("--steam-root") ?? SteamLocator.SteamRoot();
        var installed = FindInstalled(steamRoot);
        if (steamRoot != null) OutsideSteam(destination, steamRoot);
        var only = options.TryGetValue("--file", out var file) ? file.Replace('\\', '/') : null;
        if (only != null && !build.Files.Any(f => f.Path == only)) throw new ArgumentException($"Not a file of build {build.BuildId}: {only}.");
        var job = new BuildDownloadJob(build, destination, steamRoot, installed, only != null ? [only] : null);
        var dryRun = options.GetValueOrDefault("--dry-run") switch { null or "no" => false, "yes" => true, _ => throw new ArgumentException("--dry-run is yes or no.") };
        SignInRequest request = options.GetValueOrDefault("--sign-in") switch
        {
            null or "qr" => new QrSignIn(),
            "password" => new PasswordSignIn(options.GetValueOrDefault("--account") ?? Ask("Steam account name: "), ReadHidden("Password: ")),
            _ => throw new ArgumentException("--sign-in is qr or password."),
        };
        using var cancel = new CancellationTokenSource();
        Console.CancelKeyPress += (_, e) => { e.Cancel = true; cancel.Cancel(); };
        Console.Error.WriteLine($"HellForge downloads HELLDIVERS 2 build {build.BuildId} from Steam for your own account. " +
            "Sign in below; nothing of the sign-in is kept.");
        try
        {
            var result = Download(request, job, dryRun, cancel.Token).GetAwaiter().GetResult();
            Console.WriteLine(JsonSerializer.Serialize(result, CopyPreparer.JsonOptions));
            return 0;
        }
        catch (Exception error) when (error is SteamSignInException or DepotAccessDeniedException or HttpRequestException or TimeoutException or OperationCanceledException)
        {
            Console.Error.WriteLine("HellForge: " + (error is OperationCanceledException ? "stopped." : error.Message));
            return 1;
        }
    }

    static async Task<object> Download(SignInRequest request, BuildDownloadJob job, bool dryRun, CancellationToken cancellation)
    {
        await using var steam = await SteamDepotSession.SignInAsync(request, new TerminalPrompt(), cancellation);
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
    static void OutsideSteam(string destination, string steamRoot)
    {
        foreach (var library in LocalInstall.FindLibraries(steamRoot))
            if (LocalInstall.Within(destination, Path.Combine(library, "steamapps")))
                throw new ArgumentException("--out must be outside Steam's steamapps folders.");
    }

    static string Ask(string question)
    {
        Console.Error.Write(question);
        return Console.ReadLine()?.Trim() is { Length: > 0 } answer ? answer : throw new ArgumentException("No answer given.");
    }

    // A line typed without showing it.
    static string ReadHidden(string question)
    {
        if (Console.IsInputRedirected) throw new ArgumentException("--sign-in password needs a terminal to type into.");
        Console.Error.Write(question);
        var typed = new System.Text.StringBuilder();
        for (var key = Console.ReadKey(intercept: true); key.Key != ConsoleKey.Enter; key = Console.ReadKey(intercept: true))
        {
            if (key.Key == ConsoleKey.Backspace) { if (typed.Length > 0) typed.Length--; }
            else if (!char.IsControl(key.KeyChar)) typed.Append(key.KeyChar);
        }
        Console.Error.WriteLine();
        return typed.ToString();
    }

    sealed class TerminalPrompt : ISignInPrompt
    {
        public void ShowQr(string challenge)
        {
            Console.Error.WriteLine();
            Console.Error.Write(QrCode.Terminal(challenge));
            Console.Error.WriteLine("Open the Steam app on your phone, tap the Steam Guard shield, then the QR scanner, and scan this code.");
        }

        public Task<string> GuardCodeAsync(GuardCodeKind kind, string? emailHint, bool previousWasWrong, CancellationToken cancellation) =>
            Task.Run(() => Ask((previousWasWrong ? "That code was wrong. " : "") + (kind == GuardCodeKind.Email
                ? $"Steam Guard code from the email Steam sent to {emailHint}: " : "Steam Guard code from the Steam app: ")), cancellation);

        public void Status(string text) => Console.Error.WriteLine(text);
    }
}
