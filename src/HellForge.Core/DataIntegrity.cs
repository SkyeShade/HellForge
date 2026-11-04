namespace HellForge.Core;

public sealed record GameDataAuditResult(IReadOnlyList<string> Extras, IReadOnlyList<string> Problems);

// A session must not run on a copy whose data folder changed behind its cached verification. The audit allows the
// build's own files, settings.ini's kept original, and exactly the staged-mod names in ModStaging's journal. Anything
// else is moved out of game/data before launch; moving rather than deleting keeps a player's own patch recoverable.
public static class GameDataAudit
{
    public static GameDataAuditResult Quarantine(InstallLayout install, SupportedBuild build)
    {
        var data = Path.Combine(install.Game, "data");
        if (!Directory.Exists(data)) return new([], []);
        HashSet<string> allowed;
        try { allowed = Allowed(install, build); }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException)
        {
            return new([], ["data/: could not be audited: " + error.Message]);
        }
        var extras = FindExtras(data, allowed, out var scanError);
        var problems = new List<string>();
        if (scanError != null) problems.Add(scanError);
        if (extras.Count == 0) return new(extras, problems);
        var target = Path.Combine(install.Temp, "data-extras", DateTime.UtcNow.ToString("yyyyMMdd-HHmmss") + "-" + Guid.NewGuid().ToString("N"));
        foreach (var extra in extras)
        {
            try
            {
                var relative = extra["data/".Length..];
                var source = Path.Combine(data, relative.Replace('/', Path.DirectorySeparatorChar));
                var destination = Path.Combine(target, relative.Replace('/', Path.DirectorySeparatorChar));
                Directory.CreateDirectory(Path.GetDirectoryName(destination)!);
                if ((File.GetAttributes(source) & FileAttributes.Directory) != 0) Directory.Move(source, destination);
                else File.Move(source, destination);
            }
            catch (Exception error) when (error is IOException or UnauthorizedAccessException or ArgumentException)
            {
                problems.Add("extra: " + extra + " (could not be removed: " + error.Message + ")");
            }
        }
        return new(extras, problems);
    }

    static HashSet<string> Allowed(InstallLayout install, SupportedBuild build)
    {
        var allowed = build.Files.Where(file => file.Path.StartsWith("data/", StringComparison.Ordinal))
            .Select(file => file.Path["data/".Length..]).ToHashSet(StringComparer.Ordinal);
        allowed.Add("settings.ini" + GameSettings.OriginalSuffix);
        allowed.UnionWith(ModStaging.Recorded(install));
        return allowed;
    }

    static List<string> FindExtras(string data, HashSet<string> allowed, out string? error)
    {
        var extras = new List<string>();
        error = null;
        void Walk(string folder)
        {
            foreach (var path in Directory.EnumerateFileSystemEntries(folder))
            {
                var attributes = File.GetAttributes(path);
                var relative = Path.GetRelativePath(data, path).Replace('\\', '/');
                if ((attributes & FileAttributes.ReparsePoint) != 0)
                {
                    extras.Add("data/" + relative);
                    continue;
                }
                if ((attributes & FileAttributes.Directory) != 0)
                {
                    Walk(path);
                    continue;
                }
                if (!allowed.Contains(relative)) extras.Add("data/" + relative);
            }
        }
        try { Walk(data); }
        catch (Exception errorCaught) when (errorCaught is IOException or UnauthorizedAccessException)
        {
            error = "data/: could not be audited: " + errorCaught.Message;
        }
        extras.Sort(StringComparer.Ordinal);
        return extras;
    }
}

// Whether the previous session ended cleanly. A missing completedUtc or a job that was not empty means a force-ended
// session may have changed data while the next launch still has an old verification cache.
public static class SessionIntegrity
{
    public static bool LastEndedUnclean(InstallLayout install)
    {
        if (SessionLogs.Runs(install.Sessions).FirstOrDefault() is not { } latest) return false;
        if (latest.CompletedUtc == null || latest.Status == "cleanup_failed") return true;
        if (SessionFiles.ReadJson(Path.Combine(latest.Folder, "result.json")) is not { } result ||
            result.ValueKind != System.Text.Json.JsonValueKind.Object)
            return false;
        return SessionFiles.Text(result, "jobEmpty") == "false" ||
            SessionFiles.Text(result, "sdkShimOriginalsRestored") == "false" ||
            result.TryGetProperty("cleanupError", out _) || result.TryGetProperty("jobStopTimeout", out _) ||
            result.TryGetProperty("jobStopError", out _);
    }
}
