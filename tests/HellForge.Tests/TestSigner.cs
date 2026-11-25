using System.Security.Cryptography;
using HellForge.Core;

// One signer per test run stands in for the PC's own, and one synthetic machine id for the Linux seal's: tests never read
// HKLM or /etc/machine-id, nor make a machine key. Both test projects compile this file.
static class TestSigner
{
    public static readonly ECDsa Key = ECDsa.Create(ECCurve.NamedCurves.nistP256);
    public static readonly string MachineIdFile = Path.Combine(Path.GetTempPath(), "hellforge-test-machine-id-" + Environment.ProcessId);

    public static void Install()
    {
        SignerPublicKey.ForTests = Key.ExportSubjectPublicKeyInfo();
        File.WriteAllText(MachineIdFile, "0123456789abcdef0123456789abcdef\n");
        SnapshotSeal.MachineData.MachineIdPathForTests = MachineIdFile;
    }

    // Seals the files already in folder, as the capture does over the bytes it received: signed where the seal is checked
    // as on Windows, with the machine-bound key (sealKey, by default the one beside the snapshot folder) on a Linux host.
    public static void Seal(string folder, ulong account, DateTime capturedUtc, ECDsa? key = null, string? sealKey = null)
    {
        var progression = Path.Combine(folder, SnapshotSeal.ProgressionFile);
        var inventorySha256 = SnapshotSeal.Sha256(File.ReadAllBytes(Path.Combine(folder, SnapshotSeal.InventoryFile)));
        var progressionSha256 = File.Exists(progression) ? SnapshotSeal.Sha256(File.ReadAllBytes(progression)) : null;
        if (SnapshotSeal.LinuxHost)
            SnapshotSeal.WriteMachineSealed(folder, sealKey ?? SnapshotSeal.KeyPathFor(folder), account, capturedUtc, inventorySha256, progressionSha256);
        else SnapshotSeal.Write(folder, key ?? Key, account, capturedUtc, inventorySha256, progressionSha256);
    }

    // The seal checked as on Windows (linux false) or on a Linux host, the backend under Wine among them, for the call.
    public static void As(bool linux, Action action)
    {
        var saved = SnapshotSeal.LinuxHostForTests;
        SnapshotSeal.LinuxHostForTests = linux;
        try { action(); }
        finally { SnapshotSeal.LinuxHostForTests = saved; }
    }
}
