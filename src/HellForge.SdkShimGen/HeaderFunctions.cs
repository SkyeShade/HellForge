using System.Text.RegularExpressions;

namespace HellForge.SdkShimGen;

// IsOutput: the function writes through it (_Out* or _Inout*). WriteOnly: _Out* only, so a stub may clear it.
public sealed record Parameter(string Text, string Name, bool IsOutput, bool WriteOnly);

// One exported function as declared in the SDK header: "<return>\n<API macro>\n<Name>(<parameters>) [noexcept];".
public sealed record HeaderFunction(string Name, string ReturnType, string CallingConvention, IReadOnlyList<Parameter> Parameters, string Suffix);

public static partial class HeaderFunctions
{
    [GeneratedRegex(@"^[ \t]*(?<ret>[A-Za-z_][\w \t\*]*?)[ \t]*\r?\n[ \t]*(?<api>PARTY_API|PFMULTIPLAYER_API)[ \t]*\r?\n[ \t]*(?<name>[A-Za-z_]\w*)[ \t]*\(", RegexOptions.Multiline)]
    private static partial Regex Declaration();

    [GeneratedRegex(@"([A-Za-z_]\w*)\s*(\[[^\]]*\])?\s*$")]
    private static partial Regex TrailingIdentifier();

    [GeneratedRegex(@"//[^\n]*|/\*.*?\*/", RegexOptions.Singleline)]
    private static partial Regex Comment();

    public static IReadOnlyList<HeaderFunction> Parse(string headerText)
    {
        var text = Comment().Replace(headerText, "");
        var functions = new List<HeaderFunction>();
        foreach (Match match in Declaration().Matches(text))
        {
            // Parameters can contain parentheses (SAL annotations such as _Outptr_result_buffer_(*count)).
            var open = match.Index + match.Length - 1;
            var close = open;
            for (var depth = 0; close < text.Length; close++)
            {
                if (text[close] == '(') depth++;
                else if (text[close] == ')' && --depth == 0) break;
            }
            var end = text.IndexOf(';', close);
            if (close >= text.Length || end < 0) throw new FormatException("Unterminated declaration of " + match.Groups["name"].Value);
            functions.Add(new(match.Groups["name"].Value, Normalize(match.Groups["ret"].Value), match.Groups["api"].Value,
                SplitParameters(text[(open + 1)..close]), Normalize(text[(close + 1)..end])));
        }
        return functions;
    }

    static IReadOnlyList<Parameter> SplitParameters(string list)
    {
        var parameters = new List<Parameter>();
        var start = 0;
        for (int i = 0, depth = 0; i <= list.Length; i++)
        {
            if (i < list.Length && list[i] == '(') depth++;
            else if (i < list.Length && list[i] == ')') depth--;
            else if (i == list.Length || (list[i] == ',' && depth == 0))
            {
                if (ParseParameter(list[start..i]) is { } parameter) parameters.Add(parameter);
                start = i + 1;
            }
        }
        return parameters;
    }

    // One parameter from its text ("void" and the empty tail become null); IsOutput/WriteOnly from its SAL annotations.
    static Parameter? ParseParameter(string chunk)
    {
        var text = Normalize(chunk);
        if (text.Length == 0 || text == "void") return null;
        var name = TrailingIdentifier().Match(text);
        if (!name.Success) throw new FormatException("Unnamed parameter: " + text);
        var writeOnly = text.Contains("_Out", StringComparison.Ordinal);
        return new(text, name.Groups[1].Value, writeOnly || text.Contains("_Inout", StringComparison.Ordinal), writeOnly);
    }

    static string Normalize(string text) => Regex.Replace(text, @"\s+", " ").Trim();
}
