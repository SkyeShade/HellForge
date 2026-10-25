using System.Net;
using System.Net.Sockets;
using System.Runtime.InteropServices;
using System.Text.Json;

namespace HellForge.Core;

// Linux (Wine/Proton) stand-in for the Windows Filtering Platform block, which Wine cannot provide (docs/linux.md; by
// DreamyBranch and joshua-software-dev, SkyeShade/HellForge issue 1).
// The block itself is a network namespace made OUTSIDE this program by hellforge-offline.sh, which starts the
// launcher inside it (so the session runner, backend and game all inherit it) and sets HELLFORGE_LINUX_NETNS=1.
// This class never trusts that variable alone: it proves the isolation by probing, at setup and again before
// every session, and refuses to continue when an outside connection succeeds.
public static class LinuxNetworkBlock
{
    public const string EnvVar = "HELLFORGE_LINUX_NETNS";
    public static bool Requested => Environment.GetEnvironmentVariable(EnvVar) == "1";

    // Running under Wine (Proton): its ntdll exports wine_get_version, Windows' does not. An installation copied from
    // Windows keeps the Windows block's marker, so this, not the marker, says the block must be the namespace.
    public static bool UnderWine { get; } = DetectWine();

    static bool DetectWine()
    {
        try { return NativeLibrary.TryLoad("ntdll.dll", out var ntdll) && NativeLibrary.TryGetExport(ntdll, "wine_get_version", out _); }
        catch (Exception error) when (error is DllNotFoundException or BadImageFormatException or EntryPointNotFoundException) { return false; }
    }

    public static string MarkerPath(string installRoot) => new InstallLayout(installRoot).NetworkBlockMarker;

    // The installation's network block is set up for this system: the Windows block (its marker and probe), or, inside
    // the Linux namespace, the marker Install left. Setup installs it; the session runner checks it again before the game.
    // A Windows block must permit this installation's backend port alone: a marker without that port is a block from
    // before it was narrowed (all of loopback permitted), which setup places again.
    public static bool InPlace(string installRoot) =>
        File.Exists(MarkerPath(installRoot)) && (Requested ? MarkerIsEmulated(installRoot)
            : !MarkerIsEmulated(installRoot) && File.Exists(Path.Combine(installRoot, "network-block", "NetworkProbe.exe")) &&
              new InstallLayout(installRoot) is var install && install.BlockedBackendPort() == install.ComputedBackendPort);

    // True when the marker on disk was written by this class (not by the Windows script).
    public static bool MarkerIsEmulated(string installRoot)
    {
        try { return File.Exists(MarkerPath(installRoot)) && File.ReadAllText(MarkerPath(installRoot)).Contains("linux-netns", StringComparison.Ordinal); }
        catch (IOException) { return false; }
    }

    // Loopback must work (the local backend needs it) and every outside connection must fail.
    public static async Task<(bool Ok, string Detail)> Probe()
    {
        try
        {
            using var listener = new TcpListener(IPAddress.Loopback, 0);
            listener.Start();
            var port = ((IPEndPoint)listener.LocalEndpoint).Port;
            using var cts = new CancellationTokenSource(TimeSpan.FromSeconds(3));
            using var client = new TcpClient();
            await client.ConnectAsync(IPAddress.Loopback, port, cts.Token);
        }
        catch (Exception error) when (error is SocketException or OperationCanceledException or IOException)
        {
            return (false, "Loopback does not work in this environment (the local backend needs it). " +
                "linux/hellforge-linux.sh brings it up; are you starting HellForge through it?");
        }
        foreach (var target in new[] { "1.1.1.1", "8.8.8.8", "2606:4700:4700::1111" })
        {
            try
            {
                using var cts = new CancellationTokenSource(TimeSpan.FromSeconds(3));
                using var client = new TcpClient(IPAddress.Parse(target).AddressFamily);
                await client.ConnectAsync(IPAddress.Parse(target), 443, cts.Token);
                return (false, $"An outside connection to {target} succeeded, so this is not an offline environment.");
            }
            catch (Exception error) when (error is SocketException or OperationCanceledException or IOException) { }
        }
        return (true, "Outside connections are refused; loopback works.");
    }

    // Setup: prove the isolation, then leave the marker the rest of setup (and, we hope, the session runner) looks for.
    public static async Task Install(InstallLayout install)
    {
        var (ok, detail) = await Probe();
        if (!ok) throw new InvalidOperationException("The network block was not installed: " + detail);
        var path = MarkerPath(install.Root);
        Directory.CreateDirectory(Path.GetDirectoryName(path)!);
        // Same shape as the Windows marker (block, provider, filters, programs, installedUtc) plus "emulated", which
        // lets SessionRunner refuse to start outside the namespace.
        File.WriteAllText(path, JsonSerializer.Serialize(new
        {
            block = "wfp", provider = "linux-netns", emulated = "linux-netns", filters = 0, programs = 0,
            installedUtc = DateTime.UtcNow.ToString("o")
        }));
    }

    // Before every session on Linux (in the namespace, under Wine, or an installation whose block is the namespace): the
    // namespace must be there, right now. Windows: nothing.
    public static void GuardSession(string installRoot)
    {
        if (!Requested)
        {
            if (UnderWine || MarkerIsEmulated(installRoot))
                throw new InvalidOperationException("On Linux, HellForge's game runs only in its network namespace. Start HellForge with linux/hellforge-linux.sh (docs/linux.md) so the game cannot reach the internet.");
            return;
        }
        var (ok, detail) = Task.Run(Probe).GetAwaiter().GetResult();
        if (!ok) throw new InvalidOperationException("Not starting the session: " + detail);
    }
}
