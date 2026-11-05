using HellForge.Core;
using HellForge.Core.AssetMigration;
using HellForge.Core.GameData;

namespace HellForge.SteamDownload;

// Preparing a mod to share (docs/modding/assets.md): its game patch files are checked against the builds of the game on
// this PC and, when asked, against every build since release (AssetHistory, with the author's own Steam sign-in), and
// the mod is written without Arrowhead's bytes, as the author's bytes plus a recipe. The CLI (assets-check, assets-strip,
// package-mod) and the launcher's Prepare for sharing both come here.
public static class AssetSharing
{
    // Where the builds come from and what to tell the author. History: the sign-in prompt for every build since release
    // (null: this PC's builds only); it is used only when this PC's builds leave bytes the mod still ships. Saved: the
    // saved Steam sign-ins it tries first and keeps a new one in (SteamLogins; null: none, and nothing kept).
    public sealed record Context(InstallLayout? Install, ISignInPrompt? History = null, Action<string>? Log = null, CancellationToken Cancel = default,
        SteamLogins? Saved = null);

    // A check's or strip's result, the report written, and the past builds read from Steam (by label, with their day).
    public sealed record Checked(AssetKit.Result Result, string ReportPath, IReadOnlyList<string> PastBuilds);

    // A package: the zip, how many files it holds, and the check of its game patch files (null when it had none, or ships a
    // recipe already).
    public sealed record Packaged(string Zip, int Files, Checked? Assets);

    // Checks (outFolder null) or strips the mod into outFolder; the report goes to reportPath, else beside the stripped
    // mod, else into the asset kit's folder.
    public static Checked Check(string modFolder, string? outFolder, string? reportPath, Context context)
    {
        var builds = GameBuilds.Local(context.Install);
        var index = AssetKit.Index(builds, AssetKit.CacheFolder(context.Install), context.Log);
        var games = builds.Select(GameBuilds.Open).ToList();
        var named = builds.ToDictionary(b => b.Label, b => b.What, StringComparer.Ordinal);
        SteamDepotSession? session = null;
        try
        {
            if (context.History != null)
                session = AddPastBuilds(modFolder, index, games, named, context);
            var result = AssetKit.Strip(modFolder, index, games, outFolder, context.Log, label => named.GetValueOrDefault(label));
            var name = Path.GetFileName(Path.TrimEndingDirectorySeparator(modFolder));
            var report = reportPath ?? (outFolder != null ? Path.TrimEndingDirectorySeparator(outFolder) + ".asset-report.md"
                : Path.Combine(AssetKit.CacheFolder(context.Install), $"{name}.asset-report.md"));
            Directory.CreateDirectory(Path.GetDirectoryName(report)!);
            File.WriteAllText(report, result.ReportText);
            return new(result, report, [.. games.Skip(builds.Count).Select(g => named[g.Build])]);
        }
        finally
        {
            foreach (var game in games) (game as IDisposable)?.Dispose();
            session?.DisposeAsync().AsTask().GetAwaiter().GetResult();
        }
    }

    // A first match against this PC's builds; when the mod still ships bytes after it, a sign-in and the past builds that
    // hold them (AssetHistory), added to games and named. The session, or null when none was needed.
    static SteamDepotSession? AddPastBuilds(string modFolder, SourceIndex index, List<IGameResources> games, Dictionary<string, string> named, Context context)
    {
        var first = AssetKit.Strip(modFolder, index, games, null, context.Log);
        if (!first.Report.Kept.Values.Any(k => k.Count > 0)) return null;
        context.Log?.Invoke("Checking what the mod still ships against every build since release, on Steam...");
        var session = SteamDepotSession.SignInAsync(context.History!, context.Cancel, context.Saved).GetAwaiter().GetResult();
        try
        {
            var steam = SteamBuilds.For(session, context.Install, context.Cancel);
            var chunks = AssetHistory.ChunkIndex(steam, context.Log);
            foreach (var build in AssetHistory.Resolve(modFolder, index, first.Report, chunks, steam, context.Log))
            {
                games.Add(build);
                named[build.Build] = $"the build of {chunks.Builds[chunks.BuildIndex(build.Build)].Time:yyyy-MM-dd} ({build.Build}, from Steam)";
            }
            return session;
        }
        catch
        {
            session.DisposeAsync().AsTask().GetAwaiter().GetResult();
            throw;
        }
    }

    // The mod zipped to share (ModPackage) into outFolder. Its game patch files never go out with Arrowhead's bytes in
    // them: they are stripped first, into a temporary folder named as the mod's (a package is named after its folder), and
    // that copy is zipped, with the report beside the zip. A mod that already ships a recipe, or has no patch files, is
    // zipped as it is; one whose recipe would not rebuild it exactly is not zipped. A source mod is built by the caller.
    public static Packaged Package(ModManifest mod, string outFolder, Context context)
    {
        if (ModAssets.HasRecipe(mod.Folder) || AssetKit.PatchFiles(mod.Folder).Count == 0)
            return new(ModPackage.Create(mod, outFolder), ModPackage.Files(mod).Count, null);
        var report = Path.Combine(Path.GetFullPath(outFolder), ModPackage.BaseName(mod) + ".asset-report.md");
        var root = Path.Combine(Path.GetTempPath(), $"hellforge-package-{Guid.NewGuid():N}");
        try
        {
            var target = Path.Combine(root, Path.GetFileName(Path.TrimEndingDirectorySeparator(mod.Folder)));
            var check = Check(mod.Folder, target, report, context);
            if (check.Result.Outcome == AssetKit.Outcome.Failed) throw new InvalidDataException($"The mod is not packaged: {check.Result.Summary}");
            var packed = check.Result.Outcome == AssetKit.Outcome.Stripped ? ModCatalog.Parse(target, EntityProfile.Current) : mod;
            return new(ModPackage.Create(packed, outFolder), ModPackage.Files(packed).Count, check);
        }
        finally
        {
            if (Directory.Exists(root))
                try { Directory.Delete(root, recursive: true); }
                catch (Exception error) when (error is IOException or UnauthorizedAccessException) { }
        }
    }
}
