namespace HellForge.LocalBackend;

// The backend's command line (scripts/Start-LocalBoot.ps1 builds it): "--name value" pairs, each name once. Every
// option the session script passes must be listed here, or the backend refuses to start ("Backend did not become
// ready"); the tests parse the script's full set (LocalBackend.Tests, "command line").
public static class BackendOptions
{
    public static readonly string[] Names =
    [
        "--port", "--mode", "--evidence", "--startup-fixtures", "--profile-dir", "--lifetime-seconds", "--profile-template",
        "--item-catalog", "--fixture-override", "--capture-mission-bodies", "--pipe", "--sandbox", "--link", "--mod-digest", "--mod-list", "--mods", "--modset", "--directory", "--live-war", "--war", "--war-seed", "--war-subfactions", "--war-seaf",
        "--rejoin", "--premium-locked",
    ];

    public static Dictionary<string, string> Parse(IReadOnlyList<string> args)
    {
        var options = new Dictionary<string, string>(StringComparer.Ordinal);
        for (var i = 0; i < args.Count; i += 2)
            if (!Names.Contains(args[i], StringComparer.Ordinal) || i + 1 == args.Count || !options.TryAdd(args[i], args[i + 1]))
                throw new ArgumentException($"Unknown, incomplete or duplicate argument: {args[i]}.");
        return options;
    }
}
