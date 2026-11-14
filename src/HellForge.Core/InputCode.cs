namespace HellForge.Core;

// A stratagem's input code as the StratagemCode field holds it (native/mod-loader/stratagem_codes.c): one base-5 digit
// per direction, the first lowest (1 up, 2 right, 3 down, 4 left), 1 to 10 of them. Shown as arrows, typed as arrows
// or U, R, D and L.
public static class InputCode
{
    public const int MaxDirections = 10;
    const string Arrows = "↑→↓←", Letters = "URDL";

    // The directions (1..4) of a packed code, or null when it is not one.
    public static int[]? Directions(uint packed)
    {
        var directions = new List<int>();
        for (; packed != 0; packed /= 5)
        {
            if (packed % 5 == 0 || directions.Count == MaxDirections) return null;
            directions.Add((int)(packed % 5));
        }
        return directions.Count == 0 ? null : directions.ToArray();
    }

    public static uint Pack(IReadOnlyList<int> directions)
    {
        uint packed = 0;
        for (var i = directions.Count - 1; i >= 0; i--) packed = packed * 5 + (uint)directions[i];
        return packed;
    }

    public static string Text(uint packed) => Directions(packed) is { } d ? string.Join(" ", d.Select(x => Arrows[x - 1])) : "?";

    // Arrows, U R D L (spaces, commas and dashes between them ignored), or a packed number.
    public static bool TryParse(string text, out uint packed, out string? error)
    {
        packed = 0; error = null;
        if (uint.TryParse(text, System.Globalization.NumberStyles.None, System.Globalization.CultureInfo.InvariantCulture, out var number))
        {
            if (Directions(number) == null) { error = "Not a code of 1 to 10 directions."; return false; }
            packed = number;
            return true;
        }
        var directions = new List<int>();
        foreach (var c in text)
        {
            if (c is ' ' or ',' or '-') continue;
            var d = Arrows.IndexOf(c) is var a and >= 0 ? a + 1 : Letters.IndexOf(char.ToUpperInvariant(c)) + 1;
            if (d == 0) { error = "Use U, D, L and R, or the arrows."; return false; }
            directions.Add(d);
        }
        if (directions.Count is 0 or > MaxDirections) { error = "Enter 1 to 10 directions."; return false; }
        packed = Pack(directions);
        return true;
    }
}
