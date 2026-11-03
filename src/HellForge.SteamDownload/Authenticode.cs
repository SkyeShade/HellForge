using System.Runtime.InteropServices;
using System.Runtime.Versioning;
using System.Security.Cryptography.X509Certificates;

namespace HellForge.SteamDownload;

// Windows' check of a program's digital signature (Authenticode): the signature must be valid for the file as it is
// (WinVerifyTrust, the check Windows itself makes), and its signer must be Valve. Used for SteamCMD, which Valve updates
// itself, so no hash of it can be pinned.
public static class Authenticode
{
    // The signer's name when the file carries a valid signature, else null.
    [SupportedOSPlatform("windows")]
    public static X500DistinguishedName? Signer(string path)
    {
        var filePath = Marshal.StringToHGlobalUni(path);
        var file = Marshal.AllocHGlobal(Marshal.SizeOf<FileInfo>());
        try
        {
            Marshal.StructureToPtr(new FileInfo { Size = (uint)Marshal.SizeOf<FileInfo>(), FilePath = filePath }, file, false);
            var data = new TrustData
            {
                Size = (uint)Marshal.SizeOf<TrustData>(), UiChoice = NoUi, RevocationChecks = NoRevocationCheck, UnionChoice = ChoiceFile,
                File = file, StateAction = Verify,
            };
            var action = GenericVerifyV2;
            var result = WinVerifyTrust(new IntPtr(-1), ref action, ref data);
            data.StateAction = Close;
            WinVerifyTrust(new IntPtr(-1), ref action, ref data);
            if (result != 0) return null;
        }
        finally
        {
            Marshal.FreeHGlobal(file);
            Marshal.FreeHGlobal(filePath);
        }
#pragma warning disable SYSLIB0057 // the one API that reads an Authenticode signer; the signature itself was checked above
        using var signer = X509Certificate.CreateFromSignedFile(path);
#pragma warning restore SYSLIB0057
        using var certificate = new X509Certificate2(signer);
        return new X500DistinguishedName(certificate.SubjectName.RawData);
    }

    // Valve's code signing certificates name the organisation Valve (steamcmd.exe as Valve hands it out, signed in 2013:
    // "CN=Valve, O=Valve, S=Washington, C=US") or Valve Corp. (later programs). Anything else is not Valve's.
    public static bool IsValve(X500DistinguishedName? name)
    {
        if (name == null) return false;
        var organisations = name.EnumerateRelativeDistinguishedNames()
            .Where(part => part.HasMultipleElements == false && part.GetSingleElementType().Value == "2.5.4.10")
            .Select(part => part.GetSingleElementValue()).ToList();
        return organisations is [{ } organisation] && organisation is "Valve" or "Valve Corp." or "Valve Corporation";
    }

    const uint NoUi = 2, NoRevocationCheck = 0, ChoiceFile = 1, Verify = 1, Close = 2;
    static readonly Guid GenericVerifyV2 = new("00AAC56B-CD44-11d0-8CC2-00C04FC295EE");

    [StructLayout(LayoutKind.Sequential)]
    struct FileInfo
    {
        public uint Size;
        public IntPtr FilePath;
        public IntPtr Handle;
        public IntPtr KnownSubject;
    }

    [StructLayout(LayoutKind.Sequential)]
    struct TrustData
    {
        public uint Size;
        public IntPtr PolicyCallbackData;
        public IntPtr SipClientData;
        public uint UiChoice;
        public uint RevocationChecks;
        public uint UnionChoice;
        public IntPtr File;
        public uint StateAction;
        public IntPtr StateData;
        public IntPtr UrlReference;
        public uint ProviderFlags;
        public uint UiContext;
        public IntPtr SignatureSettings;
    }

    [DllImport("wintrust.dll", ExactSpelling = true)]
    static extern int WinVerifyTrust(IntPtr window, ref Guid action, ref TrustData data);
}
