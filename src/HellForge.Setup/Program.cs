using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Runtime.Intrinsics.X86;
using HellForge.Core;

namespace HellForge.Setup;

static class Program
{
    // "HellForge-Setup.exe --unpack <folder>" installs or updates without the window and without starting the launcher
    // (scripted updates and tests); the exit code is 0 on success.
    [STAThread]
    static int Main(string[] args)
    {
        if (args is ["--unpack", var folder]) return SetupForm.UnpackQuietly(folder);
        ApplicationConfiguration.Initialize();
        // Before anything else: on a Windows the launcher cannot start on, say so instead of installing it (#14).
        if (WindowsProblem() is { } problem)
        {
            if (MessageBox.Show(problem + "\n\nOpen Windows Update now?", "HellForge Setup", MessageBoxButtons.YesNo, MessageBoxIcon.Error) == DialogResult.Yes)
                try { Process.Start(new ProcessStartInfo("ms-settings:windowsupdate") { UseShellExecute = true }); }
                catch (System.ComponentModel.Win32Exception) { }
            return 3;
        }
        Application.Run(new SetupForm());
        return 0;
    }

    // WindowsSupport's facts for this PC; null (go on) when they cannot be read, and under Wine or Proton (Linux players
    // run Setup there, docs/linux.md), whose ntdll is not Windows'.
    static string? WindowsProblem()
    {
        try
        {
            if (NativeLibrary.TryLoad("ntdll.dll", out var module) && NativeLibrary.TryGetExport(module, "wine_get_version", out _)) return null;
            var ntdll = FileVersionInfo.GetVersionInfo(Path.Combine(Environment.SystemDirectory, "ntdll.dll"));
            var shadowStacks = X86Base.IsSupported && (X86Base.CpuId(7, 0).Ecx & (1 << 7)) != 0;
            return WindowsSupport.Problem(Environment.OSVersion.Version.Build,
                new Version(ntdll.FileMajorPart, ntdll.FileMinorPart, ntdll.FileBuildPart, ntdll.FilePrivatePart), shadowStacks);
        }
        catch (Exception exception) when (exception is IOException or UnauthorizedAccessException) { return null; }
    }
}
