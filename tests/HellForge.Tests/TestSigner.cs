using System.Security.Cryptography;
using HellForge.Core;

// One signer per test run stands in for the PC's own: tests never read HKLM or /var/lib/hellforge, nor make a machine key.
// Both test projects compile this file.
static class TestSigner
{
    public static readonly ECDsa Key = ECDsa.Create(ECCurve.NamedCurves.nistP256);

    public static void Install() => SignerPublicKey.ForTests = Key.ExportSubjectPublicKeyInfo();

    // Seals the files already in folder, as the capture does over the bytes it received.
    public static void Seal(string folder, ulong account, DateTime capturedUtc, ECDsa? key = null)
    {
        var progression = Path.Combine(folder, SnapshotSeal.ProgressionFile);
        SnapshotSeal.Write(folder, key ?? Key, account, capturedUtc,
            SnapshotSeal.Sha256(File.ReadAllBytes(Path.Combine(folder, SnapshotSeal.InventoryFile))),
            File.Exists(progression) ? SnapshotSeal.Sha256(File.ReadAllBytes(progression)) : null);
    }
}
