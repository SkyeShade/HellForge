using System.Text.RegularExpressions;

namespace HellForge.Core;

// Why a Sync Inventory capture that listened has read nothing, from its proxy.log (HellForge.CaptureProxy writes it; the
// Linux launcher mirrors it into the sync folder). What the launcher sees of the game (InventorySync.NothingCaptured)
// says only that it ran; the log says where its login went: never to the capture, refused at the capture's certificate,
// or stopped between the capture and the servers. Players were told to pass the title screen for all of these
// (bug-report-0495, bug-report-0510, #346). Null while the log shows none of them, and once the servers answered the
// login: what is left then is the title screen.
public static partial class CaptureStall
{
    public enum Kind { NeverConnected, CertificateRefused, Intercepted, ServersUnreachable, LoginRefused }

    public sealed record Stall(Kind Kind, string Message);

    // How long the game may run without one connection before that is a stall: the title screen comes well before.
    public static readonly TimeSpan NoConnectionGrace = TimeSpan.FromMinutes(3);

    // proxyLog: proxy.log's text. noConnectionYet: the game has run long enough (NoConnectionGrace), or the capture has
    // ended, for a log without one game connection to mean the game went past it. linux: the native Linux capture, whose
    // certificate is in HELLDIVERS 2's Proton prefix rather than Windows' store.
    public static Stall? Read(string? proxyLog, bool noConnectionYet, bool linux)
    {
        if (string.IsNullOrWhiteSpace(proxyLog)) return null;
        var facts = new Facts();
        foreach (var raw in proxyLog.Split('\n')) facts.Add(Message(raw.TrimEnd('\r')));
        return facts.LoginAnswered ? null : Decide(facts, noConnectionYet, linux);
    }

    // What the log's lines say of the game's connections, the first time each is said.
    sealed class Facts
    {
        public bool Reached, RefusedTls, LoginAnswered;
        public string? Intercepted, Unreachable, LoginStatus;

        public void Add(string line)
        {
            if (line.StartsWith("login answered", StringComparison.Ordinal)) LoginAnswered = true;
            else if (line.StartsWith(UpstreamRefused, StringComparison.Ordinal)) Intercepted ??= line[UpstreamRefused.Length..];
            else if (TlsDone().IsMatch(line)) Reached = true;
            else if (TlsFailed().IsMatch(line)) RefusedTls = true;
            else if (AskFailed().Match(line) is { Success: true } failed) Unreachable ??= failed.Groups[1].Value;
            else if (LoginStatus().Match(line) is { Success: true } login && login.Groups[1].Value[0] != '2') LoginStatus ??= login.Groups[1].Value;
        }
    }

    const string UpstreamRefused = "upstream refused: ";

    static Stall? Decide(Facts facts, bool noConnectionYet, bool linux)
    {
        var (intercepted, unreachable, loginStatus) = (facts.Intercepted, facts.Unreachable, facts.LoginStatus);
        if (intercepted != null)
            return new(Kind.Intercepted, "Something on this PC stepped into the sync's connection to HELLDIVERS 2's servers and showed its " +
                "own certificate (" + Short(intercepted) + "), so the sync cannot read your account through it. An antivirus's HTTPS or web " +
                "scanning, or a VPN or network filter that inspects traffic, usually does this: turn it off while you sync, then sync again.");
        if (unreachable != null)
            return new(Kind.ServersUnreachable, "HELLDIVERS 2 reached the sync, but the sync could not reach HELLDIVERS 2's servers (" +
                Short(unreachable) + "). A firewall, VPN or network filter that blocks HellForge's capture usually does this: allow the " +
                "capture in it or turn it off while you sync, then sync again.");
        if (loginStatus != null)
            return new(Kind.LoginRefused, "HELLDIVERS 2's servers refused the game's login (HTTP " + loginStatus + "). Check that the " +
                "normal game logs in when started from Steam (the servers may be down or in maintenance), then sync again.");
        if (facts.RefusedTls && !facts.Reached)
            return new(Kind.CertificateRefused, "HELLDIVERS 2 reached the sync but did not accept its certificate, so its login could not " +
                "go through the sync. " + (linux
                    ? "The certificate in HELLDIVERS 2's Proton prefix did not take: sync again, and send this sync's launcher.log and proxy.log if it repeats."
                    : "A security program that guards Windows' trusted certificates or scans HTTPS traffic usually does this: allow " +
                      "HellForge.CaptureProxy.exe in it, then sync again."));
        if (!facts.Reached && !facts.RefusedTls && noConnectionYet)
            return new(Kind.NeverConnected, "HELLDIVERS 2 never contacted the sync: its login went past it, straight to the servers. " +
                "A VPN, DNS changer or ad blocker that answers name lookups itself usually does this: turn it off while you sync, then sync again.");
        return null;
    }

    // A proxy.log line without its time ("12:06:48.493 hosts line added").
    static string Message(string line) => TimeStamp().Match(line) is { Success: true } time ? line[time.Length..] : line;

    static string Short(string text)
    {
        var trimmed = text.Trim().TrimEnd('.');
        return trimmed.Length <= 200 ? trimmed : trimmed[..200] + "...";
    }

    [GeneratedRegex(@"^\d{2}:\d{2}:\d{2}\.\d{3} ")]
    private static partial Regex TimeStamp();

    // "#1 TLS Tls13 TLS_AES_256_GCM_SHA384 http/1.1, sni ... in 132 ms": the game's TLS reached the capture.
    [GeneratedRegex(@"^#\d+ TLS (?!handshake failed)")]
    private static partial Regex TlsDone();

    [GeneratedRegex(@"^#\d+ TLS handshake failed")]
    private static partial Regex TlsFailed();

    // "#1 failed while asking the API after 884 ms: HttpRequestException: ..."
    [GeneratedRegex(@"^#\d+ failed while asking the API after \d+ ms: (.+)$")]
    private static partial Regex AskFailed();

    // "POST /api/Account/Login 200 52 bytes": the API's answer to the game's login.
    [GeneratedRegex(@"^POST /api/Account/Login (\d{3}) \d+ bytes")]
    private static partial Regex LoginStatus();
}
