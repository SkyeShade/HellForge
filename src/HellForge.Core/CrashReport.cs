using System.Globalization;
using System.IO.Compression;
using System.Text;
using System.Text.Json;
using System.Text.RegularExpressions;

namespace HellForge.Core;

// A crash report: one zip a player attaches to a GitHub issue after a session ended in a crash (the launcher's Save crash
// report, HellForge.Cli logs --crash-report). Nothing is sent anywhere: HellForge has no server, and the player chooses
// where the file goes. It holds
//   summary.txt  the versions, the system and the settings (SessionFacts), how the game ended, where it crashed (the mod
//                loader's crash note: code, module and offset, thread), the mods, the loader's events before the crash
//                and the last TailLines lines of its log;
//   logs/        the session's export (SessionLogs.Export) without crash dumps;
//   dump/        the crash dump, only when the player asks for it: the small one the mod loader writes at a crash, or
//                the full one with the Crash dump setting.
// Every text in it goes through ReportPrivacy, which leaves out more than the export does: it is made to be public. A
// dump is the game's memory and cannot be scrubbed, which the summary and the launcher say plainly.
public static partial class CrashReport
{
    public const string IssuePage = "https://github.com/SkyeShade/HellForge/issues/new";
    public const int TailLines = 200;
    const int EventsBefore = 20, RunnerLines = 40;

    public sealed record Saved(string Zip, int Files, int Skipped, bool Dump);

    // How the mod loader noted a crash: the exception code, the thread, where (module+offset) and the note's line in the
    // log. A fail-fast exit has no exception code ("fail-fast").
    public sealed record Fault(string Code, string Thread, string Where, string? ByModule, int Line);

    [GeneratedRegex(@"crash: (?<code>[0-9a-fA-F]{8}) on thread (?<thread>\d+) at (?<at>\S+)")]
    private static partial Regex CrashNote();

    [GeneratedRegex(@"crash: fail-fast about to end the game \(thread (?<thread>\d+), asked from (?<at>[^)]+)\)")]
    private static partial Regex FailFastNote();

    [GeneratedRegex(@"crash: by module: at (?<at>[^;]+)")]
    private static partial Regex ByModuleNote();

    // The game writes into the run folder, or into run/game when sandboxed.
    internal static IEnumerable<string> GameFolders(string run) => [run, Path.Combine(run, "game")];

    public static string? LoaderLog(string run) =>
        GameFolders(run).Select(f => Path.Combine(f, "modloader.log")).FirstOrDefault(File.Exists);

    // The crash dump the mod loader had written (crash-<process id>.dmp), or null.
    public static string? Dump(string run) =>
        GameFolders(run).Where(Directory.Exists).SelectMany(f => Directory.EnumerateFiles(f, "crash-*.dmp")).FirstOrDefault();

    // Whether a session ended in a crash: it was not stopped by the launcher, and the game left a dump, or it ended by
    // itself with an exit code other than 0 or after the mod loader noted a crash. Stopped comes first: the loader writes
    // a dump on the first first-chance fault, which the game may have handled.
    public static bool Crashed(string run)
    {
        var result = SessionFiles.ReadJson(Path.Combine(run, "result.json"));
        if (result is { } stopped && SessionFiles.Text(stopped, "observationStoppedBySignal") == "true") return false;
        if (Dump(run) != null) return true;
        if (result is not { } ended || SessionFiles.Text(ended, "gameLaunched") != "true") return false;
        if (ExitCode(ended) is { } code && code != 0) return true;
        return LoaderLog(run) is { } log && FindFault(Tail(log, int.MaxValue)) != null;
    }

    // How the game ended, for the launcher's status: "0xC0000005 (access violation)", or null when no exit code was kept.
    public static string? ExitText(string run) =>
        SessionFiles.ReadJson(Path.Combine(run, "result.json")) is { } result && ExitCode(result) is { } code
            ? $"0x{code:X8}" + (Meaning(code) is { } meaning ? $" ({meaning})" : "") : null;

    static uint? ExitCode(JsonElement result)
    {
        var text = SessionFiles.Text(result, "gameExitCode");
        if (text.StartsWith("0x", StringComparison.OrdinalIgnoreCase) &&
            uint.TryParse(text[2..], NumberStyles.AllowHexSpecifier, CultureInfo.InvariantCulture, out var hex)) return hex;
        return int.TryParse(SessionFiles.Text(result, "exitCode"), CultureInfo.InvariantCulture, out var code) ? unchecked((uint)code) : null;
    }

    // What a Windows exit code means, for the ones crashes end with, and those of a DLL the game could not start with (as
    // the session runner's Tools.ExitMeaning names them).
    public static string? Meaning(uint code) => code switch
    {
        0xC0000005 => "access violation",
        0xC0000409 => "fail-fast: a security check or abort",
        0xC00000FD => "stack overflow",
        0xC000001D => "illegal instruction",
        0xC0000374 => "heap corruption",
        0xC0000094 => "integer divide by zero",
        0xC0000142 => "a DLL failed to initialise",
        0xC0000135 => "a DLL the game needs could not be found",
        0xC000007B => "a DLL the game loads is damaged or for the wrong architecture",
        0xC0000022 => "Windows or security software denied the game access",
        0xC0000428 => "Windows blocked an unsigned DLL",
        0xC0000361 => "a Windows policy blocked a file the game loads",
        0xE06D7363 => "unhandled C++ exception",
        0x80000003 => "breakpoint",
        _ => null
    };

    // The first crash the mod loader noted, from its log's lines.
    public static Fault? FindFault(IReadOnlyList<string> lines)
    {
        for (var i = 0; i < lines.Count; i++)
        {
            if (CrashNote().Match(lines[i]) is { Success: true } crash)
                return new(crash.Groups["code"].Value.ToLowerInvariant(), crash.Groups["thread"].Value, crash.Groups["at"].Value, ByModule(lines, i), i);
            if (FailFastNote().Match(lines[i]) is { Success: true } fast)
                return new("fail-fast", fast.Groups["thread"].Value, fast.Groups["at"].Value.Trim(), null, i);
        }
        return null;
    }

    // The note's "by module" line, which follows it: the module of code outside the game.
    static string? ByModule(IReadOnlyList<string> lines, int crash)
    {
        for (var i = crash + 1; i < lines.Count && i <= crash + 2; i++)
            if (ByModuleNote().Match(lines[i]) is { Success: true } by) return by.Groups["at"].Value.Trim();
        return null;
    }

    // The last `count` lines of a log another process may still have open.
    public static List<string> Tail(string path, int count)
    {
        var lines = new Queue<string>();
        try
        {
            using var reader = new StreamReader(new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete));
            for (var line = reader.ReadLine(); line != null; line = reader.ReadLine())
            {
                lines.Enqueue(line);
                if (lines.Count > count) lines.Dequeue();
            }
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException) { }
        return [.. lines];
    }

    public static Saved Save(string run, string? gameLogs, string target, bool includeDump, Privacy? privacy = null)
    {
        var scrub = new ReportPrivacy(privacy ?? Privacy.Current, ReportPrivacy.NamesIn(run));
        var dump = includeDump ? Dump(run) : null;
        int files, skipped;
        using (var zip = ZipFile.Open(target, ZipArchiveMode.Create))
        {
            using (var writer = new StreamWriter(zip.CreateEntry("summary.txt", CompressionLevel.Optimal).Open(), new UTF8Encoding(false)))
                writer.Write(Summary(run, scrub, DateTime.UtcNow, dump != null));
            (files, skipped) = SessionLogs.AddTo(zip, run, gameLogs, new(scrub.ApplyBytes, scrub.Apply, "logs/", Dumps: false));
            if (dump != null) zip.CreateEntryFromFile(dump, "dump/" + Path.GetFileName(dump), CompressionLevel.Fastest);
        }
        return new(target, files + 1 + (dump != null ? 1 : 0), skipped, dump != null);
    }

    public static string Summary(string run, ReportPrivacy scrub, DateTime madeUtc, bool dumpIncluded)
    {
        var text = new StringBuilder();
        var result = SessionFiles.ReadJson(Path.Combine(run, "result.json"));
        var loader = LoaderLog(run) is { } log ? Tail(log, int.MaxValue) : [];
        var fault = FindFault(loader);
        Heading(text, result, run, madeUtc);
        Ending(text, result, fault, scrub);
        DumpLine(text, run, dumpIncluded);
        Facts(text, result, scrub);
        Mods(text, run, result, scrub);
        if (fault != null)
            Lines(text, "Loader events before the crash (modloader.log)", loader.Skip(Math.Max(0, fault.Line - EventsBefore)).Take(Math.Min(EventsBefore, fault.Line)), scrub);
        Lines(text, $"Session runner (session.log, last {RunnerLines} lines)", Tail(Path.Combine(run, "session.log"), RunnerLines), scrub);
        Lines(text, $"Last {TailLines} log lines (modloader.log)", loader.Skip(Math.Max(0, loader.Count - TailLines)), scrub);
        return text.ToString();
    }

    static void Heading(StringBuilder text, JsonElement? result, string run, DateTime madeUtc)
    {
        text.AppendLine("HellForge crash report");
        text.AppendLine("Read this file before you post it. HellForge left out your user folder, your PC's name, Steam ids, IP");
        text.AppendLine("addresses and the names of other players its own logs give; the game's logs can name players in ways");
        text.AppendLine("HellForge does not know. Nothing was sent anywhere: you choose where this file goes.");
        text.AppendLine();
        text.AppendLine($"Made {madeUtc.ToString("u", CultureInfo.InvariantCulture)} by HellForge {AppVersion.Text}");
        var status = result is { } r ? SessionFiles.Text(r, "status") : "";
        text.AppendLine($"Session {Path.GetFileName(Path.TrimEndingDirectorySeparator(run))}: started {Field(result, "startedUtc")}, " +
            $"ended {Field(result, "completedUtc")}, status {(status.Length > 0 ? status : "not recorded")}");
    }

    static string Field(JsonElement? result, string name) =>
        result is { } r && SessionFiles.Text(r, name) is { Length: > 0 } value && value != "null" ? value : "unknown";

    static void Ending(StringBuilder text, JsonElement? result, Fault? fault, ReportPrivacy scrub)
    {
        var code = result is { } r ? ExitCode(r) : null;
        text.AppendLine(code is { } c ? $"Game exit code: 0x{c:X8}{(Meaning(c) is { } meaning ? " (" + meaning + ")" : "")}" : "Game exit code: not recorded");
        if (result is { } state && SessionFiles.Text(state, "observationStoppedBySignal") == "true") text.AppendLine("The session was stopped from the launcher.");
        if (result is { } failed && SessionFiles.Text(failed, "failure") is { Length: > 0 } failure && failure != "null")
            text.AppendLine("Failure: " + scrub.Apply(failure));
        text.AppendLine(fault == null ? "Crash: the mod loader noted none"
            : scrub.Apply($"Crash: {fault.Code} at {fault.Where} on thread {fault.Thread}" + (fault.ByModule != null ? $" (by module: {fault.ByModule})" : "")));
    }

    static void DumpLine(StringBuilder text, string run, bool included)
    {
        var dump = Dump(run);
        text.AppendLine(dump == null ? "Crash dump: none (the game did not crash in a way that writes one)"
            : included ? $"Crash dump: included as dump/{Path.GetFileName(dump)}. A dump is the game's memory as it crashed and cannot be scrubbed: " +
                "it can hold your Steam account name and your friends' names. Only share it with someone you trust."
            : "Crash dump: not included (one was written; it is in the session's logs folder)");
    }

    static void Facts(StringBuilder text, JsonElement? result, ReportPrivacy scrub)
    {
        text.AppendLine();
        var facts = result is { } r && r.TryGetProperty("report", out var report) ? SessionFacts.From(report) : null;
        if (facts == null)
        {
            text.AppendLine("Versions and settings: not recorded (a session from before HellForge recorded them)");
            text.AppendLine(scrub.ApplyNames("This PC now: " + SystemFacts.Windows() + "; " +
                string.Join("; ", SystemFacts.Gpus().Select(g => $"{g.Name}, driver {g.Driver}"))));
            return;
        }
        foreach (var line in facts.Lines()) text.AppendLine(scrub.ApplyNames(line));
    }

    static void Mods(StringBuilder text, string run, JsonElement? result, ReportPrivacy scrub)
    {
        var loaded = new List<string>();
        var problems = new List<string>();
        if (result is { } r && r.TryGetProperty("mods", out var mods) && mods.ValueKind == JsonValueKind.Object)
        {
            loaded = Strings(mods, "loaded");
            problems = Strings(mods, "problems");
        }
        var lines = SessionFacts.ModLines(loaded, SessionFiles.ReadJson(Path.Combine(run, "modset.json")));
        text.AppendLine();
        text.AppendLine($"Mods ({lines.Count}):");
        foreach (var line in lines) text.AppendLine(scrub.ApplyNames(line));
        foreach (var problem in problems) text.AppendLine(scrub.Apply("Mod not loaded: " + problem));
    }

    static List<string> Strings(JsonElement parent, string name) =>
        parent.TryGetProperty(name, out var array) && array.ValueKind == JsonValueKind.Array
            ? [.. array.EnumerateArray().Where(e => e.ValueKind == JsonValueKind.String).Select(e => e.GetString()!)] : [];

    static void Lines(StringBuilder text, string title, IEnumerable<string> lines, ReportPrivacy scrub)
    {
        text.AppendLine();
        text.AppendLine(title + ":");
        var any = false;
        foreach (var line in lines) { text.AppendLine(scrub.Apply(line)); any = true; }
        if (!any) text.AppendLine("(none)");
    }
}
