using System.Net;
using System.Runtime.InteropServices;

namespace HellForge.Core;

// Whether the game's own log (--log-to-file, the Engine log setting) can be opened on this PC. The engine names its log
// file console_<date>-<time>-<random>-<host name>.txt, with the host name from gethostname: text in the Windows code
// page for non-Unicode programs (exe 0x5D7CD0). It then reads the whole path as UTF-8 (exe 0x59100 counts, 0x59160
// converts). On a byte that cannot start a UTF-8 character (0x80-0xBF, 0xF8-0xFF) the converter neither moves on nor
// stops: it writes the last character again and again up the stack, until it overwrites the path it reads. A PC name
// such as ゲーミングPC (Shift-JIS) or 张三的电脑 (GBK) does that, and the open's error path then crashes at
// helldivers2.exe+0x5ce339 (#424, #423, #263). With such a name the session starts the game without its log.
public static class EngineLogGuard
{
    const int Utf8CodePage = 65001;

    // Why the engine log cannot run with this host name and code page, or null. A UTF-8 code page hands the engine valid
    // UTF-8; any other turns a name outside ASCII into bytes the converter may stall on.
    public static string? Blocker(string? hostName, int ansiCodePage)
    {
        if (string.IsNullOrEmpty(hostName) || hostName.All(c => c < 0x80) || ansiCodePage == Utf8CodePage) return null;
        return "this PC's name has letters outside A-Z, 0-9 and \"-\", and the game's own log crashes the game with such a " +
            "name in its file name (helldivers2.exe+0x5ce339). Engine log is off for this session; rename the PC with " +
            "plain letters to have the engine log.";
    }

    // The same for this PC: its host name and, on Windows, its code page for non-Unicode programs (elsewhere, under
    // Wine, it is not known and counts as not UTF-8).
    public static string? Current()
    {
        string? host;
        try { host = Dns.GetHostName(); }
        catch (Exception error) when (error is System.Net.Sockets.SocketException or InvalidOperationException) { host = Environment.MachineName; }
        return Blocker(host, OperatingSystem.IsWindows() ? (int)GetACP() : 0);
    }

    [DllImport("kernel32.dll")]
    static extern uint GetACP();
}
