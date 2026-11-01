using System.ComponentModel;
using System.Diagnostics;
using System.Formats.Tar;
using System.IO.Compression;
using System.Text;
using HellForge.Core;

namespace HellForge.SteamDownload;

// SteamCMD on Linux, for the native launcher. Linux has no counterpart of the Windows signature check, so HellForge fetches
// SteamCMD's archive only from Valve's https URL (LinuxArchive; HttpClient checks the server's certificate and never
// follows a redirect from https to http) and checks the archive before anything in it is unpacked or started: only plain
// files and folders, every name inside SteamCMD's folder, about the size Valve's is, and steamcmd.sh and linux32/steamcmd
// being what they are in Valve's archive (a bash script, and a 32-bit x86 Linux program). The same two are checked again
// after SteamCMD has updated itself. Valve's archive today (2.4 MB, unchanged since 2018) holds steamcmd.sh and
// linux32/{steamcmd, steamerrorreporter, libstdc++.so.6, crashhandler.so}. A GUI program has no terminal for SteamCMD to
// ask for the password on, so the launcher opens a terminal window for it (RunInTerminalAsync, SteamCmdTerminal): the
// player types into that window, and HellForge never reads from it.
public sealed partial class SteamCmd
{
    const string LinuxScript = "steamcmd.sh";
    const string LinuxProgram = "linux32/steamcmd";
    // Valve's archive unpacks to about 7 MB in 5 files.
    const long LinuxArchiveBytes = 64L << 20;
    const int LinuxArchiveEntries = 256;
    static readonly TimeSpan TerminalTimeout = TimeSpan.FromSeconds(30);
    const string StatusName = "hellforge-terminal.status";

    public const string NoTerminalFound = "no terminal program was found to open a window for it (such as Konsole, GNOME Terminal or xterm).";

    // Checks SteamCMD's Linux archive (gzipped tar) before it is unpacked; throws SteamCmdException saying what is wrong.
    public static void CheckLinuxArchive(Stream gzipped)
    {
        using var unzipped = new GZipStream(gzipped, CompressionMode.Decompress, leaveOpen: true);
        using var reader = new TarReader(unzipped);
        var found = new HashSet<string>(StringComparer.Ordinal);
        long bytes = 0;
        var entries = 0;
        try
        {
            while (reader.GetNextEntry() is { } entry)
            {
                if (++entries > LinuxArchiveEntries) throw Refused("it holds far more files than Valve's");
                bytes += CheckEntry(entry, found);
                if (bytes > LinuxArchiveBytes) throw Refused("it is far larger than Valve's");
            }
        }
        catch (Exception error) when (error is InvalidDataException or FormatException or EndOfStreamException)
        {
            throw Refused("it is not a readable gzipped tar archive");
        }
        if (!found.Contains(LinuxScript) || !found.Contains(LinuxProgram)) throw Refused("it lacks steamcmd.sh or linux32/steamcmd");
    }

    // One entry of the archive: a folder or a plain file inside SteamCMD's folder; steamcmd.sh and linux32/steamcmd (found:
    // the ones seen) once each, and what they should be. Returns the bytes it unpacks to.
    static long CheckEntry(TarEntry entry, HashSet<string> found)
    {
        var name = entry.Name.StartsWith("./", StringComparison.Ordinal) ? entry.Name[2..] : entry.Name;
        if (entry.EntryType == TarEntryType.Directory)
        {
            if (name.TrimEnd('/').Length > 0) CheckName(name.TrimEnd('/'));
            return 0;
        }
        if (entry.EntryType == TarEntryType.GlobalExtendedAttributes) return 0; // a pax header: metadata, no file
        if (entry.EntryType is not (TarEntryType.RegularFile or TarEntryType.V7RegularFile))
            throw Refused($"{Shown(name)} is not a plain file ({entry.EntryType})");
        CheckName(name);
        if (name is not (LinuxScript or LinuxProgram)) return entry.Length;
        if (!found.Add(name)) throw Refused($"it holds {name} twice");
        var head = new byte[64];
        var read = entry.DataStream == null ? 0 : ReadHead(entry.DataStream, head);
        var script = name == LinuxScript;
        if (!(script ? IsScript(head, read) : IsLinux32Program(head, read)))
            throw Refused($"its {name} is not {(script ? "a bash script" : "a 32-bit x86 Linux program")}");
        return entry.Length;
    }

    static SteamCmdException Refused(string why) =>
        new($"the SteamCMD archive HellForge got from Valve is not what Valve's SteamCMD has: {why}. Nothing of it was unpacked or started.");

    // A name inside SteamCMD's folder: relative, with no empty, "." or ".." part, no backslash and no control character.
    static void CheckName(string name)
    {
        if (name.Length == 0 || name.StartsWith('/') || name.Contains('\\') || name.Any(char.IsControl) ||
            name.Split('/').Any(part => part is "" or "." or ".."))
            throw Refused($"{Shown(name)} would be written outside its folder");
    }

    static string Shown(string name) => new([.. name.Take(80).Select(c => char.IsControl(c) ? '?' : c)]);

    static int ReadHead(Stream stream, byte[] head)
    {
        var read = 0;
        while (read < head.Length && stream.Read(head, read, head.Length - read) is var count and > 0) read += count;
        return read;
    }

    // Valve's steamcmd.sh starts "#!/usr/bin/env bash"; a plain bash or sh shebang is taken too.
    static bool IsScript(byte[] head, int read)
    {
        var first = Encoding.ASCII.GetString(head, 0, read).Split('\n')[0].TrimEnd('\r', ' ', '\t');
        return first is "#!/usr/bin/env bash" or "#!/bin/bash" or "#!/usr/bin/bash" or "#!/bin/sh" or "#!/usr/bin/env sh";
    }

    // An ELF file for 32-bit little-endian x86 (ELFCLASS32, ELFDATA2LSB, EM_386), as linux32/steamcmd is.
    static bool IsLinux32Program(byte[] head, int read) =>
        read >= 20 && head[0] == 0x7F && head[1] == (byte)'E' && head[2] == (byte)'L' && head[3] == (byte)'F' && head[4] == 1 && head[5] == 1 &&
        head[18] == 3 && head[19] == 0;

    // tar keeps the execute bits, but an unpacking that drops them would leave SteamCMD unable to start.
    void MakeRunnable()
    {
        if (OperatingSystem.IsWindows()) return;
        const UnixFileMode execute = UnixFileMode.UserExecute | UnixFileMode.GroupExecute | UnixFileMode.OtherExecute;
        foreach (var path in new[] { LinuxScript, LinuxProgram }.Select(name => Path.Combine(Folder, name)).Where(File.Exists))
            File.SetUnixFileMode(path, File.GetUnixFileMode(path) | UnixFileMode.UserRead | execute);
    }

    // After unpacking and after SteamCMD updated itself: steamcmd.sh and linux32/steamcmd are there, inside SteamCMD's
    // folder (a link only when it leads to a file in the folder), and are a bash script and a 32-bit x86 Linux program.
    // Otherwise steamcmd.sh is deleted and nothing is started, as an unsigned steamcmd.exe is on Windows.
    void CheckLinuxProgram()
    {
        var real = LocalInstall.NoLinks(Folder, Folder);
        foreach (var (name, expected, what) in new (string, Func<byte[], int, bool>, string)[]
            { (LinuxScript, IsScript, "a bash script"), (LinuxProgram, IsLinux32Program, "a 32-bit x86 Linux program") })
        {
            var path = Path.Combine(Folder, name);
            var info = new FileInfo(path);
            string? problem = null;
            if (!info.Exists) problem = $"its {name} is missing";
            else if (info.LinkTarget != null && info.ResolveLinkTarget(returnFinalTarget: true) is var target &&
                (target == null || !(LocalInstall.Within(target.FullName, Folder) || LocalInstall.Within(target.FullName, real))))
                problem = $"its {name} leads outside its folder";
            else
            {
                var head = new byte[64];
                int read;
                using (var file = File.OpenRead(path)) read = ReadHead(file, head);
                if (!expected(head, read)) problem = $"its {name} is not {what}";
            }
            if (problem == null) continue;
            if (File.Exists(Program)) File.Delete(Program);
            throw new SteamCmdException($"the SteamCMD HellForge got is not what Valve's SteamCMD has ({problem}), so it was not started.");
        }
    }

    // What the terminal window says above SteamCMD's own lines.
    const string TerminalWords = "HellForge started Valve's SteamCMD in this window to download HELLDIVERS 2's supported build for your " +
        "account. When SteamCMD asks, type your Steam password here, and a Steam Guard code if Steam asks for one: they go to SteamCMD, " +
        "not to HellForge. This window closes when SteamCMD is done.";

    // The terminal runs this with /bin/sh; it is never written to a file. It notes its own process id beside the status
    // file, shows TerminalWords, runs SteamCMD with HOME in SteamCMD's own folder (as Start does, so SteamCMD never reaches
    // the player's ~/.steam), then notes SteamCMD's exit code. Its values come as arguments ($1 SteamCMD's folder, $2 HOME,
    // $3 the status file, $4 the words, then SteamCMD and its arguments), so no shell splits or reads them.
    internal const string TerminalScript =
        "cd \"$1\" || exit 1; home=$2; status=$3; echo $$ > \"$status.pid\"; printf '%s\\n\\n' \"$4\"; shift 4; " +
        "HOME=$home \"$@\"; code=$?; echo $code > \"$status.partial\" && mv -f \"$status.partial\" \"$status\"";

    // The terminal command line that runs SteamCMD with arguments: /bin/sh, the script, and its values.
    internal IReadOnlyList<string> TerminalCommand(IReadOnlyList<string> arguments) =>
        ["/bin/sh", "-c", TerminalScript, "hellforge-steamcmd", Folder, Home, Path.Combine(Folder, StatusName), TerminalWords, Program, .. arguments];

    // Linux: runs SteamCMD in a terminal window for the player to type into, trying each terminal in turn until one has
    // started it (opened: the one that did), and returns SteamCMD's exit code once it has ended: -1 when its window was
    // closed before. Cancelled, it ends SteamCMD and so its window.
    public async Task<int> RunInTerminalAsync(IReadOnlyList<Terminal> terminals, IReadOnlyList<string> arguments, Action<Terminal>? opened,
        CancellationToken cancellation)
    {
        if (terminals.Count == 0) throw new SteamCmdException(NoTerminalFound);
        Directory.CreateDirectory(Home);
        var status = Path.Combine(Folder, StatusName);
        ClearStatus(status);
        var command = TerminalCommand(arguments);
        try
        {
            foreach (var terminal in terminals)
            {
                if (await OpenAsync(terminal, command, status, cancellation) is not { } shell) continue;
                opened?.Invoke(terminal);
                return await WaitForTerminalAsync(shell, status, cancellation);
            }
            throw new SteamCmdException($"no terminal window could be opened for it ({string.Join(", ", terminals.Select(t => t.Name))} ended without one).");
        }
        finally { ClearStatus(status); }
    }

    // Opens the terminal's window with command; returns the id of the script it runs, or null when the terminal ended
    // with an error before starting it (the next one is tried). One that starts nothing in time is stopped and not
    // followed by another, so no second window opens. The terminal's own output goes where the launcher's does: a pipe
    // the launcher stopped reading would end the terminal, and its window, when it next writes.
    async Task<int?> OpenAsync(Terminal terminal, IReadOnlyList<string> command, string status, CancellationToken cancellation)
    {
        var start = new ProcessStartInfo(terminal.Program) { UseShellExecute = false, WorkingDirectory = Folder };
        foreach (var argument in terminal.CommandLine(command)) start.ArgumentList.Add(argument);
        Process? window;
        try { window = Process.Start(start); }
        catch (Win32Exception) { return null; }
        if (window == null) return null;
        using (window)
        {
            var clock = Stopwatch.StartNew();
            try
            {
                while (true)
                {
                    if (ReadNumber(status + ".pid") is { } shell) return shell;
                    if (window.HasExited && window.ExitCode != 0) return null;
                    if (clock.Elapsed > TerminalTimeout)
                    {
                        EndWindow(window);
                        throw new SteamCmdException($"{terminal.Name} opened no window with SteamCMD in it within {TerminalTimeout.TotalSeconds:0} seconds.");
                    }
                    await Task.Delay(TimeSpan.FromMilliseconds(250), cancellation);
                }
            }
            catch (OperationCanceledException)
            {
                EndWindow(window);
                throw;
            }
        }
    }

    // Waits until the script has noted SteamCMD's exit code (then until no SteamCMD from this folder is left) and returns
    // it; -1 when the script ended without one, because its window was closed. Cancelled, it ends the script with SteamCMD.
    async Task<int> WaitForTerminalAsync(int shell, string status, CancellationToken cancellation)
    {
        try
        {
            int? code;
            while ((code = Ended(shell, status)) == null) await Task.Delay(TimeSpan.FromMilliseconds(500), cancellation);
            while (Running()) await Task.Delay(TimeSpan.FromSeconds(1), cancellation);
            return code.Value;
        }
        catch (OperationCanceledException)
        {
            EndShell(shell, status);
            Stop();
            throw;
        }
    }

    // SteamCMD's exit code once the script noted it; -1 when the script ended without one (its window was closed: the
    // terminal hung up on SteamCMD, and anything left of it is stopped here); null while it runs.
    int? Ended(int shell, string status)
    {
        if (ReadNumber(status) is { } code) return code;
        if (ShellRunning(shell, status)) return null;
        if (ReadNumber(status) is { } last) return last;
        Stop();
        return -1;
    }

    // Ends the script and what it runs (SteamCMD), which closes its window.
    static void EndShell(int shell, string status)
    {
        if (!ShellRunning(shell, status)) return;
        try
        {
            using var script = Process.GetProcessById(shell);
            script.Kill(entireProcessTree: true);
        }
        catch (Exception error) when (error is ArgumentException or InvalidOperationException or Win32Exception) { }
    }

    // The script is running while a process with its id has the status file on its command line (an id can be reused).
    static bool ShellRunning(int shell, string status)
    {
        try { return File.ReadAllText(Path.Combine("/proc", shell.ToString(System.Globalization.CultureInfo.InvariantCulture), "cmdline")).Contains(status, StringComparison.Ordinal); }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException) { return false; }
    }

    static int? ReadNumber(string path)
    {
        try { return File.Exists(path) && int.TryParse(File.ReadAllText(path).Trim(), out var number) ? number : null; }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException) { return null; }
    }

    static void ClearStatus(string status)
    {
        foreach (var path in new[] { status, status + ".pid", status + ".partial" })
            try { File.Delete(path); }
            catch (Exception error) when (error is IOException or UnauthorizedAccessException) { }
    }

    static void EndWindow(Process window)
    {
        try { if (!window.HasExited) window.Kill(entireProcessTree: true); }
        catch (Exception error) when (error is InvalidOperationException or Win32Exception) { }
    }
}
