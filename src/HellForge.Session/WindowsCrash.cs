using System.Diagnostics;
using System.Globalization;
using System.Net;
using System.Runtime.InteropServices;
using System.Text;
using System.Text.Json.Nodes;
using System.Text.RegularExpressions;
using HellForge.Core;

namespace HellForge.Session;

// Windows' own record of a game crash: Windows Error Reporting's "Application Error" event 1000 in the Application log,
// which names the module the game crashed in and the offset into it. A game that crashes while starting (#203, #113:
// 0xC0000005 before HellForge could connect it) does so before the mod loader's crash notes start (once game.dll is
// decoded), so this is the only record of where. Read through the event log's API (wevtapi, which wevtutil uses too);
// any user may read the Application log. There is none when Windows Error Reporting is off, when the game handled its
// own crash, or under Wine.
static partial class WindowsCrash
{
    // Module: its file name ("unknown" when the crash was outside any module); Offset: "0x..." into it; Code: the
    // exception ("0xC0000005"); ModulePath: as Windows wrote it ("" when it wrote none).
    public sealed record Record(string Module, string Offset, string Code, string ModulePath);

    // Whose a module is: Windows' (its drivers included), the installation's (the copied game, HellForge's stand-ins, mod
    // loader and mods) or another program's; Unknown without a path.
    public enum Owner { Unknown, Windows, Installation, Other }

    // The modules a session puts in the game under the game's or HellForge's own names, wherever Windows says they were.
    static readonly string[] InstallationModules = ["helldivers2.exe", "game.dll", "steam_api64.dll", "steam_api64_hellforge.dll",
        "PartyWin.dll", "PlayFabMultiplayerWin.dll", "hellforge_modloader.dll"];

    // An exit code Windows Error Reporting may have recorded as a crash: an exception (0xC...), not an exit the game chose.
    public static bool IsCrash(int exitCode) => unchecked((uint)exitCode) >= 0xC0000000;

    [GeneratedRegex(@"<Event[\s>].*?</Event>", RegexOptions.Singleline)]
    private static partial Regex EventPattern();

    [GeneratedRegex(@"<Data(?:\s+Name=['""](?<name>[^'""]*)['""])?\s*(?:/>|>(?<value>[^<]*)</Data>)")]
    private static partial Regex DataPattern();

    // The crash of process pid as Windows recorded it since sinceUtc, waiting up to `wait` for the record; null when there
    // is none or it cannot be read.
    public static Record? Find(int pid, DateTime sinceUtc, TimeSpan wait)
    {
        if (!OperatingSystem.IsWindows() || LinuxNetworkBlock.UnderWine) return null;
        var since = sinceUtc.ToUniversalTime().ToString("yyyy-MM-dd'T'HH:mm:ss.fff'Z'", CultureInfo.InvariantCulture);
        var query = "*[System[Provider[@Name='Application Error'] and (EventID=1000) and TimeCreated[@SystemTime>='" + since + "']]]";
        var clock = Stopwatch.StartNew();
        while (true)
        {
            try { if (Parse(Events(query), pid) is { } found) return found; }
            catch (Exception error) when (error is DllNotFoundException or EntryPointNotFoundException) { return null; }
            // Windows Error Reporting writes it before the game's process ends; the event log may take a moment more
            if (clock.Elapsed >= wait) return null;
            Thread.Sleep(500);
        }
    }

    const int QueryChannelPath = 0x1, QueryReverseDirection = 0x200, RenderEventXml = 1;

    // The Application log's events that match query, newest first and at most 20, as their XML; "" when there are none
    // or the log cannot be read.
    static string Events(string query)
    {
        var results = EvtQuery(0, "Application", query, QueryChannelPath | QueryReverseDirection);
        if (results == 0) return "";
        var xml = new StringBuilder();
        try
        {
            var events = new nint[20];
            if (!EvtNext(results, events.Length, events, 1000, 0, out var returned)) return "";
            for (var i = 0; i < returned; i++)
            {
                xml.Append(Render(events[i]));
                EvtClose(events[i]);
            }
        }
        finally { EvtClose(results); }
        return xml.ToString();
    }

    // One event as XML; the first call asks for the size it needs.
    static string Render(nint handle)
    {
        EvtRender(0, handle, RenderEventXml, 0, 0, out var size, out _);
        if (size <= 0) return "";
        var buffer = Marshal.AllocHGlobal(size);
        try { return EvtRender(0, handle, RenderEventXml, size, buffer, out _, out _) ? Marshal.PtrToStringUni(buffer) ?? "" : ""; }
        finally { Marshal.FreeHGlobal(buffer); }
    }

    [DllImport("wevtapi.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    static extern nint EvtQuery(nint session, string path, string query, int flags);

    [DllImport("wevtapi.dll", SetLastError = true)]
    static extern bool EvtNext(nint results, int size, [Out] nint[] events, int timeout, int flags, out int returned);

    [DllImport("wevtapi.dll", SetLastError = true)]
    static extern bool EvtRender(nint context, nint fragment, int flags, int bufferSize, nint buffer, out int used, out int properties);

    [DllImport("wevtapi.dll")]
    static extern bool EvtClose(nint handle);

    // The crash of the game's process pid among events' XML (as the event log renders them, and wevtutil qe /f:xml
    // writes them), or null. Event 1000's data is named on Windows 10 and 11 (AppName, ModuleName, ExceptionCode,
    // FaultingOffset, ProcessId, ModulePath), and only in that order on older systems.
    public static Record? Parse(string xml, int pid)
    {
        foreach (Match one in EventPattern().Matches(xml))
        {
            var data = DataPattern().Matches(one.Value)
                .Select(d => (Name: d.Groups["name"].Value, Value: WebUtility.HtmlDecode(d.Groups["value"].Value).Trim())).ToList();
            var named = data.Any(d => d.Name.Length > 0);
            string Field(string name, int position) =>
                named ? data.FirstOrDefault(d => d.Name == name).Value ?? "" : position < data.Count ? data[position].Value : "";
            if (Number(Field("ProcessId", 8)) != pid || !Field("AppName", 0).Equals("helldivers2.exe", StringComparison.OrdinalIgnoreCase)) continue;
            var module = Field("ModuleName", 3);
            return new(module.Length > 0 ? module : "unknown", "0x" + Hex(Field("FaultingOffset", 7)).ToLowerInvariant(),
                "0x" + Hex(Field("ExceptionCode", 6)).ToUpperInvariant(), Field("ModulePath", 11));
        }
        return null;
    }

    // "0x35f8" or "13816"; -1 when neither.
    static long Number(string text) =>
        text.StartsWith("0x", StringComparison.OrdinalIgnoreCase) && long.TryParse(text[2..], NumberStyles.AllowHexSpecifier, CultureInfo.InvariantCulture, out var hex) ? hex
        : long.TryParse(text, NumberStyles.None, CultureInfo.InvariantCulture, out var number) ? number : -1;

    // A hex number without its 0x and leading zeros ("000000000002f3c1" -> "2f3c1").
    static string Hex(string text)
    {
        var digits = (text.StartsWith("0x", StringComparison.OrdinalIgnoreCase) ? text[2..] : text).TrimStart('0');
        return digits.Length > 0 ? digits : "0";
    }

    // Whose the crashed module is: by its path under Windows' folder or the installation, or its name (a folder reached
    // through a link reads differently in Windows' record).
    public static Owner OwnerOf(Record crash, string installRoot, string windowsFolder)
    {
        if (InstallationModules.Contains(crash.Module, StringComparer.OrdinalIgnoreCase) || Under(crash.ModulePath, installRoot)) return Owner.Installation;
        if (crash.ModulePath.Length == 0) return Owner.Unknown;
        return Under(crash.ModulePath, windowsFolder) ? Owner.Windows : Owner.Other;
    }

    static bool Under(string path, string folder)
    {
        var root = folder.TrimEnd('\\', '/');
        return root.Length > 0 && path.Length > root.Length && path.StartsWith(root, StringComparison.OrdinalIgnoreCase) && path[root.Length] is '\\' or '/';
    }

    // The record as result.json keeps it (windowsCrash).
    public static JsonObject ToJson(Record crash, Owner owner) => new()
    {
        ["module"] = crash.Module, ["offset"] = crash.Offset, ["code"] = crash.Code, ["modulePath"] = crash.ModulePath,
        ["owner"] = owner.ToString().ToLowerInvariant()
    };
}
