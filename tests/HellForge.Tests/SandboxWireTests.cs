using System.Diagnostics;
using System.Text.Json;
using HellForge.Core;

// F.4d's native seam: request validation, the self-test record and process identity. The native copy-back is covered by
// SandboxCopyBackTests. All fixture paths are synthetic; no game, Steam or Proton is started.
static class SandboxWireTests
{
    public static void Register(Action<string, Action> test, Action<bool, string> check, Action<Action> reject, string root)
    {
        test("Sandbox request validation drops covered binds and refuses changed paths, keys and runner identity", () =>
        {
            if (!OperatingSystem.IsLinux()) { Console.WriteLine("  (skipped: Linux sandbox request)"); return; }
            var fixture = MakeFixture(Path.Combine(root, "sandbox-request"));
            var valid = Request(fixture);
            var plan = SandboxProtocol.Validate(fixture.Install, fixture.Run, fixture.Prefix, valid);
            check(plan.GameOut == Path.Combine(fixture.Run, "game") && plan.ReadOnlyBinds.Count == 2 &&
                plan.Launch.RelayPort == valid.RelayPort && plan.Launch.RelayToken == valid.RelayToken,
                "A valid request was refused or normalized differently.");
            var linkedBind = Path.Combine(fixture.Install.Mods, "linked-mod");
            Directory.CreateDirectory(fixture.Install.Mods);
            File.CreateSymbolicLink(linkedBind, fixture.Outside);
            reject(() => SandboxProtocol.Validate(fixture.Install, fixture.Run, fixture.Prefix,
                Change(valid, r => r.ReadOnlyBinds = [fixture.SessionHost, fixture.Loader, linkedBind])));
            var modFolder = Directory.CreateDirectory(Path.Combine(fixture.Install.Mods, "nested")).FullName;
            var modFile = Path.Combine(modFolder, "nested.dll");
            File.WriteAllText(modFile, "mod");
            var nested = SandboxProtocol.Validate(fixture.Install, fixture.Run, fixture.Prefix,
                Change(valid, r => r.ReadOnlyBinds = [fixture.SessionHost, fixture.Loader, modFolder, modFile]));
            check(nested.ReadOnlyBinds.Count == 3 && nested.ReadOnlyBinds.Contains(modFolder, StringComparer.Ordinal) &&
                !nested.ReadOnlyBinds.Contains(modFile, StringComparer.Ordinal) &&
                nested.ReadOnlyBinds.SequenceEqual(nested.ReadOnlyBinds.OrderBy(path => path, StringComparer.Ordinal)),
                "A folder and a file inside it were not reduced to one sorted bind.");

            reject(() => SandboxProtocol.Validate(fixture.Install, fixture.Run, fixture.Prefix, Change(valid, r => r.SchemaVersion = 2)));
            reject(() => SandboxProtocol.Validate(fixture.Install, fixture.Run, fixture.Prefix, Change(valid, r => r.RunId = "not-a-run-id")));
            reject(() => SandboxProtocol.Validate(fixture.Install, Path.Combine(fixture.Install.Sessions, "not-this-run"), fixture.Prefix, valid));
            reject(() => SandboxProtocol.Validate(fixture.Install, fixture.Run, fixture.Prefix, Change(valid, r => r.GameOut = Path.Combine(fixture.Run, "other"))));
            reject(() => SandboxProtocol.Validate(fixture.Install, fixture.Run, fixture.Prefix, Change(valid, r => r.GameExe = Path.Combine(fixture.Install.Root, "other.exe"))));
            reject(() => SandboxProtocol.Validate(fixture.Install, fixture.Run, fixture.Prefix, Change(valid, r => r.DomainHost = Path.Combine(fixture.Install.Root, "other.exe"))));
            reject(() => SandboxProtocol.Validate(fixture.Install, fixture.Run, fixture.Prefix, Change(valid, r => r.Nonce = "00")));
            reject(() => SandboxProtocol.Validate(fixture.Install, fixture.Run, fixture.Prefix, Change(valid, r => r.RelayPort = 0)));
            reject(() => SandboxProtocol.Validate(fixture.Install, fixture.Run, fixture.Prefix, Change(valid, r => r.RelayToken = "not-a-token")));
            reject(() => SandboxProtocol.Validate(fixture.Install, fixture.Run, fixture.Prefix, Change(valid, r => r.Environment["LD_PRELOAD"] = "/tmp/x")));
            reject(() => SandboxProtocol.Validate(fixture.Install, fixture.Run, fixture.Prefix, Change(valid, r => r.NativeFolders = [fixture.Outside])));
            var duplicate = SandboxProtocol.Validate(fixture.Install, fixture.Run, fixture.Prefix,
                Change(valid, r => r.ReadOnlyBinds = [fixture.SessionHost, fixture.Loader, fixture.SessionHost]));
            check(duplicate.ReadOnlyBinds.Count == 2 && duplicate.ReadOnlyBinds.Contains(fixture.SessionHost, StringComparer.Ordinal),
                "A duplicated read-only bind was not dropped.");
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

        test("The Linux self-test record is bound to the app build, kernel, mode and runtime and only a current pass offers the sandbox", () =>
        {
            if (!OperatingSystem.IsLinux()) { Console.WriteLine("  (skipped: Linux sandbox readiness)"); return; }
            var install = new InstallLayout(Path.Combine(root, "sandbox-readiness"));
            install.Create(SupportedBuild.Current.BuildId);
            var sandbox = Path.Combine(install.App, "linux", "hf-sandbox");
            Directory.CreateDirectory(Path.GetDirectoryName(sandbox)!);
            File.WriteAllText(sandbox, "synthetic");
            File.SetUnixFileMode(sandbox, UnixFileMode.UserRead | UnixFileMode.UserWrite | UnixFileMode.UserExecute);
            var paths = new InstallSessionPaths(install);
            var host = new LinuxSandboxSelfTestMode(LinuxSandboxReadiness.ModeSchemaVersion, "host", "", "", "host");
            var container = new LinuxSandboxSelfTestMode(LinuxSandboxReadiness.ModeSchemaVersion, "container",
                "steamrt4", "4.0.20261021.265997", "/runtime|ABC");
            var previousMode = Environment.GetEnvironmentVariable("HELLFORGE_STEAM_RUNTIME_MODE");
            var previousIdentity = Environment.GetEnvironmentVariable("HELLFORGE_STEAM_RUNTIME_IDENTITY");
            try
            {
                Environment.SetEnvironmentVariable("HELLFORGE_STEAM_RUNTIME_MODE", "host");
                check(!SessionRunner.CanSandbox(paths), "A missing self-test result offered the sandbox.");
                LinuxSandboxReadiness.Save(install, true, "", appBuild: LinuxSandboxReadiness.AppBuild(),
                    kernelRelease: LinuxSandboxReadiness.KernelRelease(), mode: host);
                check(LinuxSandboxReadiness.Problem(install, host) == null && SessionRunner.CanSandbox(paths),
                    "A current pass did not offer the sandbox.");
                check(SessionRunner.SandboxByDefault(paths), "A current pass did not make the default session sandboxed.");
                var current = LinuxSandboxReadiness.Read(install)!;
                check(LinuxSandboxReadiness.IsCurrent(install, current), "The current self-test record is stale.");
                LinuxSandboxReadiness.Save(install, false, "Landlock ABI 1 is too old", appBuild: LinuxSandboxReadiness.AppBuild(),
                    kernelRelease: LinuxSandboxReadiness.KernelRelease(), mode: host);
                check(LinuxSandboxReadiness.Problem(install, host) == "Landlock ABI 1 is too old" && !SessionRunner.CanSandbox(paths),
                    "A current failure was offered or lost its reason.");
                LinuxSandboxReadiness.Save(install, true, "", appBuild: "another build",
                    kernelRelease: LinuxSandboxReadiness.KernelRelease(), mode: host);
                check(LinuxSandboxReadiness.Problem(install, host)!.Contains("another HellForge build", StringComparison.Ordinal) &&
                    !SessionRunner.CanSandbox(paths), "A pass from another build was accepted.");
                LinuxSandboxReadiness.Save(install, true, "", appBuild: LinuxSandboxReadiness.AppBuild(),
                    kernelRelease: "another kernel", mode: host);
                check(LinuxSandboxReadiness.Problem(install, host)!.Contains("another HellForge build or kernel", StringComparison.Ordinal) &&
                    !SessionRunner.CanSandbox(paths), "A pass from another kernel was accepted.");

                // A host pass is stale in container mode, a runtime hash change is stale, a version that did not parse still
                // counts (the hash identifies the runtime), and a runtime with no readable VERSIONS.txt is a failure.
                LinuxSandboxReadiness.Save(install, true, "", appBuild: LinuxSandboxReadiness.AppBuild(),
                    kernelRelease: LinuxSandboxReadiness.KernelRelease(), mode: host);
                check(LinuxSandboxReadiness.Problem(install, container)!.Contains("host mode", StringComparison.Ordinal),
                    "A host pass was accepted for a container session.");
                LinuxSandboxReadiness.Save(install, true, "", appBuild: LinuxSandboxReadiness.AppBuild(),
                    kernelRelease: LinuxSandboxReadiness.KernelRelease(), mode: container);
                check(LinuxSandboxReadiness.Problem(install, container) == null, "A current container pass was refused.");
                var stored = LinuxSandboxReadiness.Read(install)!;
                check(stored.Mode == "container" && stored.RuntimeName == "steamrt4" &&
                    stored.RuntimeVersion == "4.0.20261021.265997" && stored.RuntimeIdentity == "/runtime|ABC",
                    "The container mode, name, version or identity was not stored.");
                var changed = container with { RuntimeIdentity = "/runtime|DEF" };
                check(LinuxSandboxReadiness.Problem(install, changed)!.Contains("another Steam Linux Runtime", StringComparison.Ordinal),
                    "A changed runtime hash was accepted.");
                var unknown = container with { RuntimeVersion = "unknown" };
                LinuxSandboxReadiness.Save(install, true, "", appBuild: LinuxSandboxReadiness.AppBuild(),
                    kernelRelease: LinuxSandboxReadiness.KernelRelease(), mode: unknown);
                check(LinuxSandboxReadiness.Problem(install, unknown) == null,
                    "A container pass whose VERSIONS.txt version did not parse was refused although its hash identifies it.");
                var noHash = container with { RuntimeVersion = "unknown", RuntimeIdentity = "/runtime|no-hash" };
                LinuxSandboxReadiness.Save(install, true, "", appBuild: LinuxSandboxReadiness.AppBuild(),
                    kernelRelease: LinuxSandboxReadiness.KernelRelease(), mode: noHash);
                check(LinuxSandboxReadiness.Problem(install, noHash)!.Contains("could not identify", StringComparison.Ordinal),
                    "A container pass without a readable VERSIONS.txt was accepted.");

                File.WriteAllText(LinuxSandboxReadiness.ModeFileFor(install),
                    JsonSerializer.Serialize(host, SandboxJson.Default.LinuxSandboxSelfTestMode));
                var evaluatedHost = LinuxSandboxReadiness.EvaluateSelfTest(install, 0, "");
                check(evaluatedHost.Passed && evaluatedHost.Mode.Mode == "host", "A valid host mode file was not a pass.");
                File.Delete(LinuxSandboxReadiness.ModeFileFor(install));
                var missingMode = LinuxSandboxReadiness.EvaluateSelfTest(install, 0, "");
                check(!missingMode.Passed && missingMode.Reason.Contains("did not report", StringComparison.Ordinal),
                    "A self-test that exited 0 without a mode file was recorded as a pass.");

                Environment.SetEnvironmentVariable("HELLFORGE_STEAM_RUNTIME_MODE", "container");
                Environment.SetEnvironmentVariable("HELLFORGE_STEAM_RUNTIME_IDENTITY", "/runtime|ABC");
                var unknownMetadata = unknown with { RuntimeName = "steamrt4", RuntimeIdentity = "/runtime|ABC" };
                File.WriteAllText(LinuxSandboxReadiness.ModeFileFor(install),
                    JsonSerializer.Serialize(unknownMetadata, SandboxJson.Default.LinuxSandboxSelfTestMode));
                check(LinuxSandboxReadiness.EvaluateSelfTest(install, 0, "").Passed,
                    "A container self-test whose version did not parse was refused although its hash identifies it.");
                Environment.SetEnvironmentVariable("HELLFORGE_STEAM_RUNTIME_IDENTITY", "/runtime|no-hash");
                File.WriteAllText(LinuxSandboxReadiness.ModeFileFor(install),
                    JsonSerializer.Serialize(noHash, SandboxJson.Default.LinuxSandboxSelfTestMode));
                var unnamed = LinuxSandboxReadiness.EvaluateSelfTest(install, 0, "");
                check(!unnamed.Passed && unnamed.Reason.Contains("could not identify", StringComparison.Ordinal),
                    "A container self-test without a readable VERSIONS.txt was recorded as a pass.");

                File.WriteAllText(LinuxSandboxReadiness.FileFor(install),
                    $"{{\"schemaVersion\":1,\"appBuild\":\"{LinuxSandboxReadiness.AppBuild()}\",\"kernelRelease\":\"{LinuxSandboxReadiness.KernelRelease()}\",\"passed\":true,\"reason\":\"\",\"checkedUtc\":\"{DateTime.UtcNow:O}\"}}");
                check(LinuxSandboxReadiness.Problem(install, host)!.Contains("old schema", StringComparison.Ordinal),
                    "A schema-1 record was accepted.");
            }
            finally
            {
                Environment.SetEnvironmentVariable("HELLFORGE_STEAM_RUNTIME_MODE", previousMode);
                Environment.SetEnvironmentVariable("HELLFORGE_STEAM_RUNTIME_IDENTITY", previousIdentity);
            }
        });

        test("The installed app build matches AppVersion.Text and a known publish", () =>
        {
            var install = new InstallLayout(Path.Combine(root, "installed-app-build"));
            install.Create(SupportedBuild.Current.BuildId);
            var text = AppVersion.Text;
            var split = text.LastIndexOf(" (", StringComparison.Ordinal);
            var version = split < 0 ? text : text[..split];
            var commit = split < 0 ? "" : text[(split + 2)..^1];
            File.WriteAllText(Path.Combine(install.App, "app-build.json"),
                JsonSerializer.Serialize(new { version, commit }));
            check(LinuxSandboxReadiness.InstalledAppBuild(install) == AppVersion.Text,
                "The installed app-build.json did not produce the runner's AppVersion.Text.");
            File.WriteAllText(Path.Combine(install.App, "app-build.json"),
                JsonSerializer.Serialize(new { version = "0.3.0-beta", commit = "c4a1506deadbeef" }));
            check(LinuxSandboxReadiness.InstalledAppBuild(install) == "0.3.0-beta (c4a1506)",
                "The installed app build did not use the version and first seven commit characters.");
            check(LinuxSandboxReadiness.SelfTestReason("first\nhf-sandbox: Landlock is too old\n", 1) == "hf-sandbox: Landlock is too old" &&
                LinuxSandboxReadiness.SelfTestReason("last line\n", 7) == "last line",
                "The self-test reason did not match the launcher's wording.");
            File.WriteAllText(Path.Combine(install.App, "app-build.json"), "{}");
            reject(() => LinuxSandboxReadiness.InstalledAppBuild(install));
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
        RelayPort = 40400,
        RelayToken = Guid.NewGuid().ToString("N"),
        GameArguments = ["--release"],
        Environment = new(StringComparer.Ordinal)
        {
            ["APPDATA"] = @"Z:\run\game\environment\Roaming", ["HELLFORGE_PIPE"] = "hellforge-test"
        },
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
            RelayPort = request.RelayPort, RelayToken = request.RelayToken,
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
            RelayPort = mutable.RelayPort, RelayToken = mutable.RelayToken,
            GameArguments = mutable.GameArguments, Environment = mutable.Environment, ReadOnlyBinds = mutable.ReadOnlyBinds,
            NativeFolders = mutable.NativeFolders, RunnerPid = mutable.RunnerPid, RunnerStartedUtc = mutable.RunnerStartedUtc
        };
    }

    sealed class MutableRequest
    {
        public int SchemaVersion = 1;
        public string RunId = "", Nonce = "", InstallRoot = "", InstallRootWine = "", DomainHost = "", GameExe = "", GameExeWine = "",
            GameOut = "", GameOutWine = "", GameDllWine = "", BundleDirectoryWine = "", WorkingDirectoryWine = "", AppRootWine = "",
            BackendOrigin = "", RelayToken = "", RunnerStartedUtc = "";
        public List<string> GameArguments = [], ReadOnlyBinds = [], NativeFolders = [];
        public Dictionary<string, string> Environment = new(StringComparer.Ordinal);
        public int RelayPort, RunnerPid;
    }
}
