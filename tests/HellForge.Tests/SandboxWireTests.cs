using System.Diagnostics;
using HellForge.Core;

// F.4d's native seam: request validation, the self-test record and process identity. The native copy-back is covered by
// SandboxCopyBackTests. All fixture paths are synthetic; no game, Steam or Proton is started.
static class SandboxWireTests
{
    public static void Register(Action<string, Action> test, Action<bool, string> check, Action<Action> reject, string root)
    {
        test("Sandbox request validation refuses each changed path, bind, environment key and runner identity", () =>
        {
            if (!OperatingSystem.IsLinux()) { Console.WriteLine("  (skipped: Linux sandbox request)"); return; }
            var fixture = MakeFixture(Path.Combine(root, "sandbox-request"));
            var valid = Request(fixture);
            var plan = SandboxProtocol.Validate(fixture.Install, fixture.Run, fixture.Prefix, valid);
            check(plan.GameOut == Path.Combine(fixture.Run, "game") && plan.ReadOnlyBinds.Count == 2,
                "A valid request was refused or normalized differently.");
            var linkedBind = Path.Combine(fixture.Install.Mods, "linked-mod");
            Directory.CreateDirectory(fixture.Install.Mods);
            File.CreateSymbolicLink(linkedBind, fixture.Outside);
            reject(() => SandboxProtocol.Validate(fixture.Install, fixture.Run, fixture.Prefix,
                Change(valid, r => r.ReadOnlyBinds = [fixture.SessionHost, fixture.Loader, linkedBind])));

            reject(() => SandboxProtocol.Validate(fixture.Install, fixture.Run, fixture.Prefix, Change(valid, r => r.SchemaVersion = 2)));
            reject(() => SandboxProtocol.Validate(fixture.Install, fixture.Run, fixture.Prefix, Change(valid, r => r.RunId = "not-a-run-id")));
            reject(() => SandboxProtocol.Validate(fixture.Install, Path.Combine(fixture.Install.Sessions, "not-this-run"), fixture.Prefix, valid));
            reject(() => SandboxProtocol.Validate(fixture.Install, fixture.Run, fixture.Prefix, Change(valid, r => r.GameOut = Path.Combine(fixture.Run, "other"))));
            reject(() => SandboxProtocol.Validate(fixture.Install, fixture.Run, fixture.Prefix, Change(valid, r => r.GameExe = Path.Combine(fixture.Install.Root, "other.exe"))));
            reject(() => SandboxProtocol.Validate(fixture.Install, fixture.Run, fixture.Prefix, Change(valid, r => r.DomainHost = Path.Combine(fixture.Install.Root, "other.exe"))));
            reject(() => SandboxProtocol.Validate(fixture.Install, fixture.Run, fixture.Prefix, Change(valid, r => r.Nonce = "00")));
            reject(() => SandboxProtocol.Validate(fixture.Install, fixture.Run, fixture.Prefix, Change(valid, r => r.Environment["LD_PRELOAD"] = "/tmp/x")));
            reject(() => SandboxProtocol.Validate(fixture.Install, fixture.Run, fixture.Prefix, Change(valid, r => r.NativeFolders = [fixture.Outside])));
            reject(() => SandboxProtocol.Validate(fixture.Install, fixture.Run, fixture.Prefix, Change(valid, r => r.ReadOnlyBinds = [fixture.SessionHost, fixture.SessionHost])));
            reject(() => SandboxProtocol.Validate(fixture.Install, fixture.Run, fixture.Prefix, Change(valid, r => r.ReadOnlyBinds = [fixture.Loader])));
            reject(() => SandboxProtocol.Validate(fixture.Install, fixture.Run, fixture.Prefix, Change(valid, r => r.ReadOnlyBinds = [fixture.SessionHost, fixture.Outside])));
            reject(() => SandboxProtocol.Validate(fixture.Install, fixture.Run, fixture.Prefix, Change(valid, r => r.ReadOnlyBinds = null!)));
            reject(() => SandboxProtocol.Validate(fixture.Install, fixture.Run, fixture.Prefix, Change(valid, r => r.Environment = null!)));
            reject(() => SandboxProtocol.Validate(fixture.Install, fixture.Run, fixture.Prefix, Change(valid, r => r.BackendOrigin = "http://192.0.2.1:40000")));
            reject(() => SandboxProtocol.Validate(fixture.Install, fixture.Run, fixture.Prefix, Change(valid, r => r.RunnerPid = 0)));
            reject(() => SandboxProtocol.Validate(fixture.Install, fixture.Run, fixture.Prefix, Change(valid, r => r.RunnerStartedUtc = "yesterday")));
            File.Delete(Path.Combine(fixture.Install.App, "linux", "hf-sandbox"));
            reject(() => SandboxProtocol.Validate(fixture.Install, fixture.Run, fixture.Prefix, valid));
        });

        test("The Linux self-test record is bound to the app build and kernel and only a current pass offers the sandbox", () =>
        {
            if (!OperatingSystem.IsLinux()) { Console.WriteLine("  (skipped: Linux sandbox readiness)"); return; }
            var install = new InstallLayout(Path.Combine(root, "sandbox-readiness"));
            install.Create(SupportedBuild.Current.BuildId);
            var sandbox = Path.Combine(install.App, "linux", "hf-sandbox");
            Directory.CreateDirectory(Path.GetDirectoryName(sandbox)!);
            File.WriteAllText(sandbox, "synthetic");
            File.SetUnixFileMode(sandbox, UnixFileMode.UserRead | UnixFileMode.UserWrite | UnixFileMode.UserExecute);
            var paths = new InstallSessionPaths(install);
            check(!SessionRunner.CanSandbox(paths), "A missing self-test result offered the sandbox.");
            LinuxSandboxReadiness.Save(install, true, "", appBuild: LinuxSandboxReadiness.AppBuild(), kernelRelease: LinuxSandboxReadiness.KernelRelease());
            check(LinuxSandboxReadiness.Problem(install) == null && SessionRunner.CanSandbox(paths), "A current pass did not offer the sandbox.");
            check(SessionRunner.SandboxByDefault(paths), "A current pass did not make the default session sandboxed.");
            var current = LinuxSandboxReadiness.Read(install)!;
            check(LinuxSandboxReadiness.IsCurrent(current), "The current self-test record is stale.");
            LinuxSandboxReadiness.Save(install, false, "Landlock ABI 1 is too old", appBuild: LinuxSandboxReadiness.AppBuild(),
                kernelRelease: LinuxSandboxReadiness.KernelRelease());
            check(LinuxSandboxReadiness.Problem(install) == "Landlock ABI 1 is too old" && !SessionRunner.CanSandbox(paths),
                "A current failure was offered or lost its reason.");
            LinuxSandboxReadiness.Save(install, true, "", appBuild: "another build", kernelRelease: LinuxSandboxReadiness.KernelRelease());
            check(LinuxSandboxReadiness.Problem(install)!.Contains("another HellForge build", StringComparison.Ordinal) && !SessionRunner.CanSandbox(paths),
                "A pass from another build was accepted.");
            LinuxSandboxReadiness.Save(install, true, "", appBuild: LinuxSandboxReadiness.AppBuild(), kernelRelease: "another kernel");
            check(LinuxSandboxReadiness.Problem(install)!.Contains("another HellForge build or kernel", StringComparison.Ordinal) && !SessionRunner.CanSandbox(paths),
                "A pass from another kernel was accepted.");
        });

        test("Supervisor process identity: a current stand-in is alive, a dead pid is not, and a changed start time is refused", () =>
        {
            if (!OperatingSystem.IsLinux()) { Console.WriteLine("  (skipped: Linux process identity)"); return; }
            using var current = Process.GetCurrentProcess();
            var stat = LinuxProcessIdentity.Read(current.Id);
            check(stat != null && LinuxProcessIdentity.Alive(current.Id) && LinuxProcessIdentity.ParentAlive(Environment.ProcessId) &&
                LinuxProcessIdentity.Matches(current.Id, current.StartTime.ToUniversalTime()),
                "The current process was not identified.");
            const int deadPid = int.MaxValue;
            check(LinuxProcessIdentity.Read(deadPid) == null && !LinuxProcessIdentity.Alive(deadPid), "A dead pid was alive.");
            check(!LinuxProcessIdentity.Matches(current.Id, current.StartTime.ToUniversalTime().AddMinutes(-10)),
                "A reused start time was accepted.");
            using var standIn = Process.Start(new ProcessStartInfo("/bin/sleep", "30") { UseShellExecute = false })!;
            var standInStarted = standIn.StartTime.ToUniversalTime();
            try
            {
                check(LinuxProcessIdentity.Alive(standIn.Id) && LinuxProcessIdentity.Matches(standIn.Id, standInStarted),
                    "A stand-in child was not identified.");
            }
            finally { standIn.Kill(); standIn.WaitForExit(); }
            check(!LinuxProcessIdentity.Matches(standIn.Id, standInStarted), "A killed child still matched.");
        });
    }

    sealed class Fixture
    {
        public required InstallLayout Install { get; init; }
        public required string Prefix { get; init; }
        public required string Run { get; init; }
        public required string SessionHost { get; init; }
        public required string Loader { get; init; }
        public required string Outside { get; init; }
    }

    static Fixture MakeFixture(string root)
    {
        if (!OperatingSystem.IsLinux()) throw new PlatformNotSupportedException("The sandbox fixture creates a Linux executable.");
        var install = new InstallLayout(root);
        install.Create(SupportedBuild.Current.BuildId);
        Directory.CreateDirectory(Path.Combine(install.Game, "bin"));
        File.WriteAllText(Path.Combine(install.Game, "bin", "helldivers2.exe"), "game");
        var sessionHost = Directory.CreateDirectory(Path.Combine(install.App, "session")).FullName;
        File.WriteAllText(Path.Combine(sessionHost, "HellForge.Session.exe"), "host");
        var loader = Path.Combine(install.App, "sdk-shims", "hellforge_modloader.dll");
        Directory.CreateDirectory(Path.GetDirectoryName(loader)!);
        File.WriteAllText(loader, "loader");
        var sandbox = Path.Combine(install.App, "linux", "hf-sandbox");
        Directory.CreateDirectory(Path.GetDirectoryName(sandbox)!);
        File.WriteAllText(sandbox, "sandbox");
        File.SetUnixFileMode(sandbox, UnixFileMode.UserRead | UnixFileMode.UserWrite | UnixFileMode.UserExecute);
        var prefix = Directory.CreateDirectory(Path.Combine(root, "prefix")).FullName;
        Directory.CreateDirectory(Path.Combine(prefix, "pfx"));
        var run = Directory.CreateDirectory(Path.Combine(install.Sessions, Guid.NewGuid().ToString("N"))).FullName;
        Directory.CreateDirectory(Path.Combine(run, "game"));
        var outside = Path.Combine(root, "outside.txt");
        File.WriteAllText(outside, "outside");
        return new() { Install = install, Prefix = prefix, Run = run, SessionHost = sessionHost, Loader = loader, Outside = outside };
    }

    static SandboxStartRequest Request(Fixture fixture) => new()
    {
        RunId = Path.GetFileName(fixture.Run),
        Nonce = Guid.NewGuid().ToString("N"),
        InstallRoot = fixture.Install.Root,
        InstallRootWine = @"C:\HellForge",
        DomainHost = Path.Combine(fixture.Install.App, "session", "HellForge.Session.exe"),
        GameExe = Path.Combine(fixture.Install.Game, "bin", "helldivers2.exe"),
        GameExeWine = @"C:\HellForge\game\bin\helldivers2.exe",
        GameOut = Path.Combine(fixture.Run, "game"),
        GameOutWine = @"Z:\run\game",
        GameDllWine = @"C:\HellForge\game\data\game\game.dll",
        BundleDirectoryWine = @"C:\HellForge\game\data",
        WorkingDirectoryWine = @"C:\HellForge\game\bin",
        AppRootWine = @"Z:\HellForge\app",
        BackendOrigin = "http://127.0.0.1:40000",
        GameArguments = ["--release"],
        Environment = new(StringComparer.Ordinal) { ["APPDATA"] = @"Z:\run\game\environment\Roaming" },
        ReadOnlyBinds = [fixture.SessionHost, fixture.Loader],
        RunnerPid = Environment.ProcessId,
        RunnerStartedUtc = Process.GetCurrentProcess().StartTime.ToUniversalTime().ToString("o")
    };

    static SandboxStartRequest Change(SandboxStartRequest request, Action<MutableRequest> change)
    {
        var mutable = new MutableRequest
        {
            SchemaVersion = request.SchemaVersion, RunId = request.RunId, Nonce = request.Nonce, InstallRoot = request.InstallRoot,
            InstallRootWine = request.InstallRootWine,
            DomainHost = request.DomainHost, GameExe = request.GameExe, GameExeWine = request.GameExeWine, GameOut = request.GameOut,
            GameOutWine = request.GameOutWine, GameDllWine = request.GameDllWine, BundleDirectoryWine = request.BundleDirectoryWine,
            WorkingDirectoryWine = request.WorkingDirectoryWine, AppRootWine = request.AppRootWine, BackendOrigin = request.BackendOrigin,
            GameArguments = [.. request.GameArguments], Environment = new(request.Environment, StringComparer.Ordinal),
            ReadOnlyBinds = [.. request.ReadOnlyBinds],
            NativeFolders = [.. request.NativeFolders], RunnerPid = request.RunnerPid, RunnerStartedUtc = request.RunnerStartedUtc
        };
        change(mutable);
        return new()
        {
            SchemaVersion = mutable.SchemaVersion, RunId = mutable.RunId, Nonce = mutable.Nonce, InstallRoot = mutable.InstallRoot,
            InstallRootWine = mutable.InstallRootWine,
            DomainHost = mutable.DomainHost, GameExe = mutable.GameExe, GameExeWine = mutable.GameExeWine, GameOut = mutable.GameOut,
            GameOutWine = mutable.GameOutWine, GameDllWine = mutable.GameDllWine, BundleDirectoryWine = mutable.BundleDirectoryWine,
            WorkingDirectoryWine = mutable.WorkingDirectoryWine, AppRootWine = mutable.AppRootWine, BackendOrigin = mutable.BackendOrigin,
            GameArguments = mutable.GameArguments, Environment = mutable.Environment, ReadOnlyBinds = mutable.ReadOnlyBinds,
            NativeFolders = mutable.NativeFolders, RunnerPid = mutable.RunnerPid, RunnerStartedUtc = mutable.RunnerStartedUtc
        };
    }

    sealed class MutableRequest
    {
        public int SchemaVersion = 1;
        public string RunId = "", Nonce = "", InstallRoot = "", InstallRootWine = "", DomainHost = "", GameExe = "", GameExeWine = "",
            GameOut = "", GameOutWine = "", GameDllWine = "", BundleDirectoryWine = "", WorkingDirectoryWine = "", AppRootWine = "",
            BackendOrigin = "", RunnerStartedUtc = "";
        public List<string> GameArguments = [], ReadOnlyBinds = [], NativeFolders = [];
        public Dictionary<string, string> Environment = new(StringComparer.Ordinal);
        public int RunnerPid;
    }
}
