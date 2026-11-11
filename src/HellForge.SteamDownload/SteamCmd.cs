using System.ComponentModel;
using System.Diagnostics;
using System.Formats.Tar;
using System.IO.Compression;
using System.Text.RegularExpressions;
using HellForge.Core;

namespace HellForge.SteamDownload;

// SteamCMD, Valve's own command-line Steam client: the way to the supported build for a player without a phone to scan
// the QR code, with no password ever typed into HellForge. HellForge fetches it from Valve into its own tools folder (never
// a Steam library); on Windows its program must carry Valve's signature (Valve updates it itself, so no hash can be pinned).
// It starts with every command filled in except the password: the account name, a download_depot per pinned depot, quit.
// The player types the password and any Steam Guard code into SteamCMD's own window, so they go to Valve's program.
// download_depot puts each depot in <SteamCMD>/steamapps/content/app_553850/depot_<id> (linux32/steamapps on Linux),
// which SteamDepotDownload reads like the Steam client's download. After every run, however it ended, ForgetSignIn
// removes what SteamCMD remembers of the sign-in; once the build is moved out, Remove deletes SteamCMD altogether.
public sealed class SteamCmd(string folder)
{
    // Valve's own downloads (developer.valvesoftware.com/wiki/SteamCMD).
    public static readonly Uri WindowsArchive = new("https://steamcdn-a.akamaihd.net/client/installer/steamcmd.zip");
    public static readonly Uri LinuxArchive = new("https://steamcdn-a.akamaihd.net/client/installer/steamcmd_linux.tar.gz");

    public string Folder { get; } = InSteamapps(folder)
        ? throw new ArgumentException("SteamCMD's folder must be outside Steam's steamapps folders.")
        : LocalInstall.FullPath(folder);

    // A path through any steamapps folder (a Steam library's), whether or not Steam's own folder is known.
    public static bool InSteamapps(string path) =>
        LocalInstall.FullPath(path).Split(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar)
            .Any(part => part.Equals("steamapps", StringComparison.OrdinalIgnoreCase));

    // In an installation's tools folder, beside the mod tools.
    public static SteamCmd In(InstallLayout install) => new(Path.Combine(install.Root, "tools", "steamcmd"));

    public string Program => Path.Combine(Folder, OperatingSystem.IsWindows() ? "steamcmd.exe" : "steamcmd.sh");

    // Linux: SteamCMD keeps its sign-in under HOME (~/Steam, ~/.steam). It runs with HOME here, so it never reaches the
    // player's own Steam folders, and forgetting the sign-in removes this folder.
    string Home => Path.Combine(Folder, "home");

    // Steam account names are letters, digits and underscores; anything else is refused, so the name can never carry a
    // second command.
    public static bool IsAccountName(string? name) => name != null && Regex.IsMatch(name, "^[A-Za-z0-9_]{1,64}$");

    // SteamCMD's command line: stop at the first failed command (a failed sign-in downloads nothing), sign in with the
    // account name only (SteamCMD then asks for the password and any Steam Guard code in its window), download each pinned
    // depot (or only those named), quit.
    public static IReadOnlyList<string> Arguments(string account, SupportedBuild build, IReadOnlyCollection<uint>? depots = null)
    {
        if (!IsAccountName(account)) throw new ArgumentException("A Steam account name has only letters, digits and underscores.");
        List<string> arguments = ["+@ShutdownOnFailedCommand", "1", "+login", account];
        foreach (var depot in build.Depots.Where(d => depots == null || depots.Contains(d.Id)))
            arguments.AddRange(["+download_depot", build.AppId, depot.Id.ToString(System.Globalization.CultureInfo.InvariantCulture), depot.ManifestId]);
        arguments.Add("+quit");
        return arguments;
    }

    // Fetches SteamCMD from Valve unless it is here; on Windows its program must be signed by Valve.
    public async Task InstallAsync(HttpClient http, CancellationToken cancellation)
    {
        if (!File.Exists(Program))
        {
            Directory.CreateDirectory(Folder);
            var windows = OperatingSystem.IsWindows();
            var archive = Path.Combine(Folder, windows ? "steamcmd.zip" : "steamcmd_linux.tar.gz");
            await using (var source = await http.GetStreamAsync(windows ? WindowsArchive : LinuxArchive, cancellation))
            await using (var file = File.Create(archive)) await source.CopyToAsync(file, cancellation);
            if (windows) ZipFile.ExtractToDirectory(archive, Folder, overwriteFiles: true);
            else
            {
                await using var unzipped = new GZipStream(File.OpenRead(archive), CompressionMode.Decompress);
                await TarFile.ExtractToDirectoryAsync(unzipped, Folder, overwriteFiles: true, cancellation);
            }
            File.Delete(archive);
        }
        CheckSigned();
    }

    // Windows: SteamCMD's program must carry a valid signature by Valve, or it is deleted and never started.
    public void CheckSigned()
    {
        if (!OperatingSystem.IsWindows() || Authenticode.IsValve(Authenticode.Signer(Program))) return;
        File.Delete(Program);
        throw new SteamCmdException("the SteamCMD program HellForge got is not signed by Valve, so it was deleted and not started.");
    }

    // SteamCMD updates itself on its first start: done here without a window (output: its lines), then its program is
    // checked again.
    public async Task UpdateAsync(Action<string>? output, CancellationToken cancellation)
    {
        using var process = Start(["+quit"], visible: false);
        process.OutputDataReceived += (_, line) => { if (!string.IsNullOrWhiteSpace(line.Data)) output?.Invoke(line.Data.Trim()); };
        process.ErrorDataReceived += (_, _) => { };
        process.BeginOutputReadLine();
        process.BeginErrorReadLine();
        await WaitAsync(process, cancellation);
        CheckSigned();
    }

    // Starts SteamCMD: visible, in a window of its own (from the launcher) or in this terminal (the CLI), for the player
    // to type into; else hidden, its output read.
    public Process Start(IReadOnlyList<string> arguments, bool visible)
    {
        var start = new ProcessStartInfo(Program) { UseShellExecute = false, CreateNoWindow = !visible, WorkingDirectory = Folder };
        foreach (var argument in arguments) start.ArgumentList.Add(argument);
        if (!OperatingSystem.IsWindows())
        {
            Directory.CreateDirectory(Home);
            start.Environment["HOME"] = Home;
        }
        start.RedirectStandardOutput = start.RedirectStandardError = start.RedirectStandardInput = !visible;
        try { return Process.Start(start) ?? throw new SteamCmdException("SteamCMD did not start."); }
        catch (Win32Exception error) { throw new SteamCmdException($"SteamCMD did not start: {error.Message}"); }
    }

    string TerminalExit => Path.Combine(Folder, "terminal-exit");
    string TerminalPid => Path.Combine(Folder, "terminal-pid");

    // Linux, from a program with no terminal of its own (the launcher): SteamCMD in a terminal window, for the player
    // to type into. A few lines of bash, given to the terminal as one argument, run it there, say which process they
    // are and leave SteamCMD's exit code in a file: some terminals hand the window to another process and end at once,
    // so the terminal's own process says nothing about SteamCMD.
    public Process StartInTerminal(IReadOnlyList<string> arguments, LinuxTerminal terminal)
    {
        Directory.CreateDirectory(Home);
        File.Delete(TerminalExit);
        File.Delete(TerminalPid);
        var start = new ProcessStartInfo(terminal.Program) { UseShellExecute = false, WorkingDirectory = Folder };
        string[] script = ["/bin/bash", "-c", TerminalScript(), "steamcmd-in-terminal"];
        foreach (var argument in terminal.Before.Concat(script).Concat(arguments)) start.ArgumentList.Add(argument);
        try { return Process.Start(start) ?? throw new SteamCmdException("the terminal window for SteamCMD did not start."); }
        catch (Win32Exception error) { throw new SteamCmdException($"the terminal window for SteamCMD did not start: {error.Message}"); }
    }

    // HOME and the folder are set here, not on the terminal's process: a terminal that hands its window to another
    // process passes neither on. One line, since some terminals send their arguments on as text. After a failure the
    // window stays until Enter, so SteamCMD's own words can be read.
    string TerminalScript() => string.Join("; ",
        "echo $$ > " + Quote(TerminalPid),
        "cd " + Quote(Folder) + " || exit 1",
        "HOME=" + Quote(Home) + " " + Quote(Program) + " \"$@\"",
        "status=$?",
        "echo $status > " + Quote(TerminalExit + ".partial") + " && mv -f " + Quote(TerminalExit + ".partial") + " " + Quote(TerminalExit),
        "if [ $status != 0 ]; then echo",
        "read -r -p \"SteamCMD ended with exit code $status. Press Enter to close this window. \" _ || true",
        "fi",
        "exit $status");

    static string Quote(string text) => "'" + text.Replace("'", "'\\''", StringComparison.Ordinal) + "'";

    // The script's own process, while it runs: the pid it wrote, checked against that process's command line.
    Process? TerminalScriptProcess()
    {
        try
        {
            if (!File.Exists(TerminalPid) || !int.TryParse(File.ReadAllText(TerminalPid).Trim(), out var pid)) return null;
            var commandLine = File.ReadAllText($"/proc/{pid}/cmdline").Split('\0');
            return commandLine.Contains(TerminalScript()) ? Process.GetProcessById(pid) : null;
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException or ArgumentException) { return null; }
    }

    // Waits for the SteamCMD that StartInTerminal started and returns its exit code, or -1 when the window was closed
    // before SteamCMD ended: no exit code, and neither the terminal nor the script alive for closedAfter (a terminal that
    // hands its window on is gone at once, a moment before the script has started). Cancelled, it stops SteamCMD.
    public async Task<int> WaitInTerminalAsync(Process terminal, CancellationToken cancellation, TimeSpan? closedAfter = null)
    {
        var grace = closedAfter ?? TimeSpan.FromSeconds(15);
        var quiet = Stopwatch.StartNew();
        try
        {
            while (!File.Exists(TerminalExit))
            {
                await Task.Delay(TimeSpan.FromMilliseconds(500), cancellation);
                using var script = TerminalScriptProcess();
                if (!terminal.HasExited || script != null) quiet.Restart();
                else if (quiet.Elapsed >= grace && !File.Exists(TerminalExit)) return -1;
            }
            while (Running()) await Task.Delay(TimeSpan.FromSeconds(1), cancellation);
            return int.TryParse(File.ReadAllText(TerminalExit).Trim(), out var code) ? code : -1;
        }
        catch (OperationCanceledException)
        {
            // The script's process tree holds SteamCMD also where the terminal's own process has already ended.
            using var script = TerminalScriptProcess();
            Stop(script);
            Stop(terminal);
            throw;
        }
    }

    // Waits until SteamCMD has ended and returns its exit code. After updating itself SteamCMD may start its new self and
    // exit, so it has ended when none of its processes from this folder is left. Cancelled, it stops SteamCMD.
    public async Task<int> WaitAsync(Process process, CancellationToken cancellation)
    {
        try
        {
            await process.WaitForExitAsync(cancellation);
            while (Running()) await Task.Delay(TimeSpan.FromSeconds(1), cancellation);
            return process.ExitCode;
        }
        catch (OperationCanceledException)
        {
            Stop(process);
            throw;
        }
    }

    public bool Running()
    {
        var any = false;
        foreach (var process in Processes()) using (process) any = true;
        return any;
    }

    // Ends SteamCMD: the process started and any of its processes from this folder.
    public void Stop(Process? started = null)
    {
        foreach (var process in (started != null ? [started] : Array.Empty<Process>()).Concat(Processes()))
            try { if (!process.HasExited) process.Kill(entireProcessTree: true); }
            catch (Exception error) when (error is Win32Exception or InvalidOperationException or NotSupportedException) { }
    }

    IEnumerable<Process> Processes()
    {
        foreach (var process in Process.GetProcessesByName("steamcmd"))
        {
            string? path = null;
            try { path = process.MainModule?.FileName; }
            catch (Exception error) when (error is Win32Exception or InvalidOperationException or NotSupportedException) { }
            if (path != null && LocalInstall.Within(Path.GetFullPath(path), Folder)) yield return process;
            else process.Dispose();
        }
    }

    // What SteamCMD remembers of a sign-in: its config folder (config.vdf keeps the sign-in token, loginusers.vdf the
    // account), userdata, its logs (they name the account), Steam Guard's sentry files (ssfn*) and, on Linux, its HOME.
    static readonly string[] Remembered = ["config", "userdata", "logs", "home"];

    // Removes what SteamCMD remembers of the sign-in; SteamCMD itself and what it downloaded stay, for another try.
    public void ForgetSignIn()
    {
        if (!Directory.Exists(Folder)) return;
        foreach (var remembered in Remembered.Select(name => Path.Combine(Folder, name)).Where(Directory.Exists))
            Directory.Delete(remembered, recursive: true);
        foreach (var sentry in Directory.EnumerateFiles(Folder, "ssfn*")) File.Delete(sentry);
    }

    // Deletes SteamCMD with everything in its folder (after the build was moved out of it).
    public void Remove()
    {
        if (Directory.Exists(Folder)) Directory.Delete(Folder, recursive: true);
    }

    // After SteamCMD ended without the whole download: why, in words a player can act on.
    public static string Unfinished(int exitCode, IReadOnlyList<SteamDepotDownload.DepotState> depots)
    {
        long done = depots.Sum(d => d.Done), total = depots.Sum(d => d.Total);
        var forgotten = " Nothing of the sign-in was kept.";
        if (done > 0)
            return $"SteamCMD closed before the download was complete ({done / 1e9:0.0} of {total / 1e9:0.0} GB). Start it again to " +
                "continue; what it already downloaded is kept." + forgotten;
        return $"SteamCMD closed without downloading the build (exit code {exitCode}). If the sign-in did not work, start it again " +
            "and type your password, and the Steam Guard code if Steam asks for one, in its window. If HELLDIVERS 2 is borrowed " +
            "through Steam Family Sharing, the account cannot download it: use the Steam app instead." + forgotten;
    }
}

// SteamCMD cannot be used (it could not be fetched, is not Valve's, or did not start): the reason in plain words.
public sealed class SteamCmdException(string message, Exception? inner = null) : Exception(message, inner);
