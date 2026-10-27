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
            var mapped = Path.GetFullPath(Path.Combine(root, NativeRelative(cRelative)));
            if (!Within(root, mapped)) throw new InvalidDataException("The Windows path escapes the Proton prefix's C: drive.");
            return mapped;
        }
        if (DrivePath(path, 'Z', out var zRelative))
        {
            var root = Path.GetPathRoot(Path.GetFullPath(prefix))!;
            return Path.GetFullPath(Path.Combine(root, NativeRelative(zRelative)));
        }
        return path;
    }

    public static string ToWinePath(string path, string prefix)
    {
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
