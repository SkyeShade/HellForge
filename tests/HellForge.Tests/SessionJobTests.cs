// The session runner's game job (src/HellForge.Session/HostBoundedProcess.cs): with singleProcess the game is alone in it
// and cannot start another program; without it, children start as before (the Crash dump diagnostic needs one).
static class SessionJobTests
{
    public static void Register(Action<string, Action> test, Action<bool, string> check, Action<Action> reject, string root)
    {
        test("Sessions: the game's job holds it alone, so it cannot start another program", () =>
        {
            if (!OperatingSystem.IsWindows()) return;
            var cmd = Path.Combine(Environment.SystemDirectory, "cmd.exe");
            int Run(bool singleProcess)
            {
                // cmd starts a second cmd that exits with 7, and exits with what it got
                using var job = new HostBoundedProcess(cmd, "/d /c \"\"" + cmd + "\" /d /c exit 7\"", Environment.SystemDirectory, "", 30, singleProcess);
                job.Resume();
                check(job.Process.WaitForExit(20000), "The test process ended.");
                return job.Process.ExitCode;
            }
            check(Run(singleProcess: false) == 7, "Without the limit a child starts.");
            var limited = Run(singleProcess: true);
            check(limited != 7, $"With the limit the child cannot start (exit code {limited}).");
        });

        test("Several installations: another installation's game lets a session start only with HELLFORGE_MULTI_INSTANCE", () =>
        {
            HellForge.Core.InstallLayout Install(string name)
            {
                var folder = Directory.CreateDirectory(Path.Combine(root, "instances", name, "game", "bin")).Parent!.Parent!.FullName;
                File.WriteAllText(Path.Combine(folder, HellForge.Core.InstallLayout.MarkerFile), "{}");
                return new HellForge.Core.InstallLayout(folder);
            }
            var one = Install("one");
            var two = Install("two");
            var twoGame = Path.Combine(two.Game, "bin", "helldivers2.exe");
            var oneGame = Path.Combine(one.Game, "bin", "helldivers2.exe");
            var normal = Path.Combine(root, "instances", "steam", "Helldivers 2", "bin", "helldivers2.exe");
            static (string, string?)[] Game(string? path) => [("helldivers2", path)];
            check(HellForge.Session.GameProcesses.OtherInstallation(one, twoGame) == two.Root, "Another installation's game is recognised.");
            check(HellForge.Session.GameProcesses.Blocking(one, [], multiInstance: false) == null, "Nothing running: the session starts.");
            check(HellForge.Session.GameProcesses.Blocking(one, Game(twoGame), multiInstance: false) != null, "Without the switch any game stops it.");
            check(HellForge.Session.GameProcesses.Blocking(one, Game(twoGame), multiInstance: true) == null, "With it, another installation's game does not.");
            check(HellForge.Session.GameProcesses.Blocking(one, Game(oneGame), multiInstance: true) != null, "This installation's own game still stops it.");
            check(HellForge.Session.GameProcesses.Blocking(one, Game(normal), multiInstance: true) != null, "The normal game still stops it.");
            check(HellForge.Session.GameProcesses.Blocking(one, Game(null), multiInstance: true) != null, "A game whose path cannot be read still stops it.");
            check(HellForge.Session.GameProcesses.Blocking(one, [("GameMon64", null)], multiInstance: true) != null, "GameGuard's monitor still stops it.");
        });

        test("Sandboxed sessions: a low-integrity game writes to the pipes and folders labelled for it, and no others", () =>
        {
            if (!OperatingSystem.IsWindows()) return;
            var cmd = Path.Combine(Environment.SystemDirectory, "cmd.exe");
            var name = "hellforge-test-" + Guid.NewGuid().ToString("N");
            HellForge.Core.PipeServer.LowIntegrityClients = true;
            using var pipe = HellForge.Core.PipeServer.Create(name, 1);
            HellForge.Core.PipeServer.LowIntegrityClients = false;
            check(HellForge.Core.PipeServer.LabelLevel(pipe.SafePipeHandle) == 0x1000, "The pipe carries a low label.");
            // made new with its label, as the session makes run/game (relabelling an existing folder needs WRITE_OWNER)
            Directory.CreateDirectory(Path.Combine(root, "sandbox-low"));
            var labelled = HellForge.Session.LowIntegrityHelper.CreateLowFolder(Path.Combine(root, "sandbox-low", "game"));
            reject(() => HellForge.Session.LowIntegrityHelper.CreateLowFolder(labelled));
            var inside = Directory.CreateDirectory(Path.Combine(labelled, "userdata", "profiles")).FullName;
            // a folder at this process's own level: refused to the game only when this process is above low (a workspace
            // labelled low runs its tests at low integrity)
            var above = HellForge.Session.LowIntegrityHelper.CurrentLevel() > 0x1000;
            var other = above ? Directory.CreateDirectory(Path.Combine(Path.GetTempPath(), "hellforge-sandbox-test-" + Guid.NewGuid().ToString("N"))).FullName : labelled;
            var token = HellForge.Session.LowIntegrityHelper.CreateLowToken();
            try
            {
                var waiting = pipe.WaitForConnectionAsync();
                var command = $"/d /c \"echo from the game> \\\\.\\pipe\\{name} & echo x> \"{Path.Combine(labelled, "written.txt")}\" & echo x> \"{Path.Combine(inside, "written.txt")}\" & echo x> \"{Path.Combine(other, "written.txt")}\"\"";
                using var job = new HostBoundedProcess(cmd, command, Environment.SystemDirectory, "", 30, token: token);
                job.Resume();
                check(waiting.Wait(20000), "The low-integrity process opened the pipe.");
                using var reader = new StreamReader(pipe);
                check(reader.ReadLine()?.Trim() == "from the game", "Its message arrived.");
                check(job.Process.WaitForExit(20000), "The test process ended.");
                check(File.Exists(Path.Combine(labelled, "written.txt")), "It wrote into the folder labelled low.");
                check(File.Exists(Path.Combine(inside, "written.txt")), "And into a folder made inside it later, which inherits the label.");
                if (above)
                    check(!File.Exists(Path.Combine(other, "written.txt")), "It could not write into a folder at the player's level.");
            }
            finally
            {
                HellForge.Session.LowIntegrityHelper.CloseToken(token);
                if (above) Directory.Delete(other, recursive: true);
            }
        });
    }
}
