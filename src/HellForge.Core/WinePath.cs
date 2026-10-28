namespace HellForge.Core;

public static class WinePath
{
    public static string PrefixFromInstallRoot(string installRoot)
    {
        for (var directory = new DirectoryInfo(Path.GetFullPath(installRoot)); directory != null; directory = directory.Parent)
            if (directory.Name.Equals("drive_c", StringComparison.OrdinalIgnoreCase) &&
                directory.Parent?.Name.Equals("pfx", StringComparison.OrdinalIgnoreCase) == true)
                return directory.Parent.Parent?.FullName ?? Path.GetPathRoot(directory.FullName)!;

        throw new InvalidOperationException("The HellForge installation must be inside a Proton prefix's pfx/drive_c folder.");
    }

    public static string ToHostPath(string path, string prefix)
    {
        if (DrivePath(path, 'C', out var cRelative))
        {
            var root = Path.GetFullPath(Path.Combine(prefix, "pfx", "drive_c"));
            // C:\\folder repeats the root separator but still means C:\folder, not a rooted host path.
            var relative = NativeRelative(cRelative).TrimStart(Path.DirectorySeparatorChar);
            var mapped = relative.Length == 0 ? root : Path.GetFullPath(Path.Combine(root, relative));
            if (!Within(root, mapped)) throw new InvalidDataException("The Windows path escapes the Proton prefix's C: drive.");
            return mapped;
        }
        if (DrivePath(path, 'Z', out var zRelative))
        {
            var root = Path.GetPathRoot(Path.GetFullPath(prefix))!;
            return Path.GetFullPath(Path.Combine(root, NativeRelative(zRelative)));
        }
        // A host path on Linux never starts with a drive letter, so an unmapped drive is a Windows path
        // with no host equivalent. Proton prefixes provide only C: and Z:.
        if (OperatingSystem.IsLinux() && WindowsDrive(path) is { } drive)
            throw new InvalidDataException($"The Windows path uses drive {drive}:, which has no host equivalent here; only C: and Z: are mapped.");
        return path;
    }

    public static string ToWinePath(string path, string prefix)
    {
        // A host path on Linux never starts with a drive letter. Mapping an already-Windows path would
        // produce a Z: path to a literal "C:\..." folder, which cannot be opened.
        if (OperatingSystem.IsLinux() && WindowsDrive(path) is { } drive)
            throw new InvalidDataException($"The path is already a Windows path on drive {drive}:, not a host path: {path}");
        var fullPath = Path.GetFullPath(path);
        var cRoot = Path.GetFullPath(Path.Combine(prefix, "pfx", "drive_c"));
        if (Within(cRoot, fullPath))
        {
            var relative = Path.GetRelativePath(cRoot, fullPath);
            return relative == "." ? @"C:\" : @"C:\" + WineRelative(relative);
        }

        var root = Path.GetPathRoot(fullPath)!;
        var fromRoot = Path.GetRelativePath(root, fullPath);
        return fromRoot == "." ? @"Z:\" : @"Z:\" + WineRelative(fromRoot);
    }

    // The drive letter of a path that starts with one (C:, D:\folder, Z:/folder), or null.
    static char? WindowsDrive(string? path)
    {
        if (path is not { Length: >= 2 } || !char.IsAsciiLetter(path[0]) || path[1] != ':') return null;
        if (path.Length > 2 && path[2] is not ('\\' or '/')) return null;
        return char.ToUpperInvariant(path[0]);
    }

    static bool DrivePath(string path, char drive, out string relative)
    {
        if (path.Length >= 2 && char.ToUpperInvariant(path[0]) == drive && path[1] == ':' &&
            (path.Length == 2 || path[2] is '\\' or '/'))
        {
            relative = path.Length <= 3 ? "" : path[3..];
            return true;
        }
        relative = "";
        return false;
    }

    static string NativeRelative(string path) => path.Replace('\\', Path.DirectorySeparatorChar).Replace('/', Path.DirectorySeparatorChar);
    static string WineRelative(string path) => path.Replace(Path.DirectorySeparatorChar, '\\').Replace('/', '\\');

    static bool Within(string root, string path)
    {
        root = Path.TrimEndingDirectorySeparator(Path.GetFullPath(root));
        path = Path.GetFullPath(path);
        return path.Equals(root, StringComparison.Ordinal) ||
            path.StartsWith(root + Path.DirectorySeparatorChar, StringComparison.Ordinal);
    }
}
