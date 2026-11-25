using HellForge.Core;

// installer and installer-copy: the installer an installation keeps of itself (KeptInstaller, docs/offline-kit.md), as
// Settings > Installation shows and copies it.
static class InstallerCommand
{
    public static object Info(Dictionary<string, string> options)
    {
        var state = KeptInstaller.Find(Launch.Install(options));
        return new
        {
            kept = state.Kept != null,
            kind = state.Kept?.KindName,
            file = state.Kept?.Path,
            version = state.Kept?.Version,
            bytes = state.Kept?.Bytes,
            sha256 = state.Kept?.Sha256,
            notKept = state.NotKept
        };
    }

    public static object Copy(Dictionary<string, string> options, string destination)
    {
        var state = KeptInstaller.Find(Launch.Install(options));
        var kept = state.Kept ?? throw new InvalidDataException("This installation keeps no installer yet" +
            (state.NotKept is { } why ? ": setup could not keep one (" + why + ")." : ": it appears after the next install or update with setup."));
        using var cancel = new CancellationTokenSource();
        Console.CancelKeyPress += (_, e) => { e.Cancel = true; cancel.Cancel(); };
        var reported = "";
        KeptInstallerCopy copy;
        try
        {
            copy = KeptInstaller.CopyTo(kept, destination, p =>
            {
                var line = $"{p.Stage}: {(p.Total > 0 ? p.Done * 100 / p.Total : 100) / 10 * 10}%";
                if (line == reported) return;
                reported = line;
                Console.Error.WriteLine(line);
            }, cancel.Token);
        }
        catch (OperationCanceledException) { throw new IOException("Stopped; the unfinished copy was deleted."); }
        return new { copied = copy.Path, file = copy.File, kind = kept.KindName, version = kept.Version, bytes = copy.Bytes, sha256 = copy.Sha256 };
    }
}
