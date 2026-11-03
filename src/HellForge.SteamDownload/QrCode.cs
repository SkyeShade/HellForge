using System.Text;
using QRCoder;

namespace HellForge.SteamDownload;

// The sign-in QR code (QRCoder), as modules for the launcher to draw and as text for a terminal.
public static class QrCode
{
    // true: a dark module. The quiet zone (4 light modules on each side) is included.
    public static bool[,] Modules(string text)
    {
        using var generator = new QRCodeGenerator();
        using var data = generator.CreateQrCode(text, QRCodeGenerator.ECCLevel.M);
        var rows = data.ModuleMatrix;
        var modules = new bool[rows.Count, rows.Count];
        for (var y = 0; y < rows.Count; y++)
            for (var x = 0; x < rows.Count; x++) modules[y, x] = rows[y][x];
        return modules;
    }

    // Two rows of modules per line in half-block characters, drawn white on black with ANSI colours so the code reads
    // the right way round on a light or dark terminal.
    public static string Terminal(string text)
    {
        var modules = Modules(text);
        var size = modules.GetLength(0);
        var lines = new StringBuilder();
        for (var y = 0; y < size; y += 2)
        {
            lines.Append("\u001b[97;40m");
            for (var x = 0; x < size; x++)
            {
                var top = !modules[y, x];
                var bottom = y + 1 < size && !modules[y + 1, x];
                lines.Append((top, bottom) switch { (true, true) => '█', (true, false) => '▀', (false, true) => '▄', _ => ' ' });
            }
            lines.Append("\u001b[0m\n");
        }
        return lines.ToString();
    }
}
