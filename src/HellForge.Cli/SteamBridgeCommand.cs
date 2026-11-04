using System.Runtime.InteropServices;
using HellForge.Core;

// The native Steam bridge the Linux scripts run instead of socat (docs/linux.md "Sessions", .deepcode/review-E5.md):
// host creates one unix socket per Steam loopback port, inner recreates those ports inside the network namespace, and
// check reports listening (0), definitely not listening (1), or cannot tell (3).
static class SteamBridgeCommand
{
    const int Interrupted = 130;
    const int Terminated = 143;

    public static int Run(string[] args)
    {
        if (!OperatingSystem.IsLinux())
        {
            Console.Error.WriteLine("HellForge: the Steam bridge runs on Linux only.");
            return 1;
        }
        if (args.Length < 2)
        {
            Usage();
            return 2;
        }
        var directory = LinuxSteamBridge.SocketDirectory();
        switch (args[1])
        {
            case "host":
                if (args.Length != 2) { Usage(); return 2; }
                return Run(directory, host: true);
            case "inner":
                if (args.Length != 2) { Usage(); return 2; }
                return Run(directory, host: false);
            case "check":
                if (args.Length > 3) { Usage(); return 2; }
                return CheckBridge(args.Length == 3 ? args[2] : null, directory);
            default:
                Usage();
                return 2;
        }
    }

    static void Usage() => Console.Error.WriteLine("Usage: HellForge.Cli steam-bridge host | inner | check [socket]");

    // Fail closed: 0 answered, 1 definitely not listening, 3 cannot tell. An unexpected failure must not look like
    // "not listening", because the session's wineserver guard starts the game on 1.
    static int CheckBridge(string? socket, string directory)
    {
        try
        {
            if (socket == null) return LinuxSteamBridge.AnyAnswers(directory) ? 0 : 1;
            var state = LinuxSteamBridge.Check(socket, out var problem);
            if (state == LinuxSteamBridge.SocketState.CannotTell)
                Console.Error.WriteLine("HellForge: cannot tell whether a bridge is listening on " + socket + ": " +
                    (problem ?? "the check did not answer") + ".");
            return (int)state;
        }
        catch (Exception error)
        {
            Console.Error.WriteLine("HellForge: cannot tell whether a bridge is listening on " + (socket ?? directory) +
                ": " + error.Message + ".");
            return (int)LinuxSteamBridge.SocketState.CannotTell;
        }
    }

    static int Run(string directory, bool host)
    {
        if (!OperatingSystem.IsLinux()) throw new PlatformNotSupportedException("The Steam bridge runs on Linux only.");
        using var stop = new CancellationTokenSource();
        var exit = 0;
        using var term = PosixSignalRegistration.Create(PosixSignal.SIGTERM, context =>
        {
            context.Cancel = true;
            exit = Terminated;
            stop.Cancel();
        });
        using var interrupt = PosixSignalRegistration.Create(PosixSignal.SIGINT, context =>
        {
            context.Cancel = true;
            exit = Interrupted;
            stop.Cancel();
        });
        try
        {
            if (host) LinuxSteamBridge.RunHost(directory, Console.WriteLine, stop.Token);
            else LinuxSteamBridge.RunInner(directory, Console.Error.WriteLine, stop.Token);
        }
        catch (OperationCanceledException) when (stop.IsCancellationRequested) { }
        return exit;
    }
}
