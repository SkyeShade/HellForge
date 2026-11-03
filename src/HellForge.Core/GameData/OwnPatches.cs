using System.Security.Cryptography;
using System.Text;

namespace HellForge.Core.GameData;

// HellForge's own patches built from the installation's game copy (WarbondLocks, ForgeCredits): each kept under
// temp/<name>/<hash of its key>/ until its key changes (the game build, what it covers), only the last one kept, and
// written as one patch of each archive it changes.
public static class OwnPatches
{
    const string DoneFile = "done";

    // The folder for key under temp/<name>/, built by build when it is not there yet (a folder is built once: its done file
    // is written last). Older folders of name go.
    public static string Cached(InstallLayout install, string name, string key, Action<string> build)
    {
        var root = Path.Combine(install.Temp, name);
        var folder = Path.Combine(root, Convert.ToHexString(SHA256.HashData(Encoding.UTF8.GetBytes(key)))[..16].ToLowerInvariant());
        if (File.Exists(Path.Combine(folder, DoneFile))) return folder;
        if (Directory.Exists(root))
            foreach (var old in Directory.GetDirectories(root)) Directory.Delete(old, recursive: true);
        Directory.CreateDirectory(folder);
        build(folder);
        File.WriteAllText(Path.Combine(folder, DoneFile), key + "\n");
        return folder;
    }

    // The resources as patch files in output, one patch of each archive (<archive>.patch_0, .gpu_resources and, when any
    // resource streams, .stream); their names.
    public static List<string> Write(Dictionary<string, Dictionary<ResourceKey, ResourceParts>> byArchive, string output)
    {
        var files = new List<string>();
        foreach (var (archive, resources) in byArchive)
        {
            var (main, gpu, stream) = ResourceArchive.Write(resources);
            var name = archive + ".patch_0";
            File.WriteAllBytes(Path.Combine(output, name), main);
            File.WriteAllBytes(Path.Combine(output, name + ".gpu_resources"), gpu);
            files.AddRange([name, name + ".gpu_resources"]);
            if (stream.Length == 0) continue;
            File.WriteAllBytes(Path.Combine(output, name + ".stream"), stream);
            files.Add(name + ".stream");
        }
        return files;
    }
}
