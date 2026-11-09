using HellForge.Core;

// The Linux wrapper truncates its log when it starts and writes plain output with no timestamps; setup reads it only when
// the ownership probe's wrapper wrote it. These samples are the lines linux/hellforge-linux.sh and
// linux/hellforge-offline.sh print when they stop.
static class OwnershipLogTests
{
    public static void Register(Action<string, Action> test, Action<bool, string> check, string root)
    {
        const string start = "HellForge: /home/player/HellForge with /home/player/Steam/Proton/proton (prefix /home/player/prefix)\n" +
            "Steam compatibility app ID: 553850\n";

        test("The Linux ownership check reports the wrapper failure", () =>
        {
            const string missingPrograms = "HellForge on Linux needs these programs, which were not found: socat ip\n" +
                "Install them with: sudo dnf install iproute socat util-linux procps-ng\n";
            check(SteamOwnershipCheck.LinuxWrapperProblem(start + missingPrograms) ==
                "HellForge on Linux needs these programs, which were not found: socat ip", "The missing-program line was not reported.");

            const string appArmor = "This system only lets allowed programs make the network namespace HellForge runs in. " +
                "Run this once, then start HellForge again:\n  sudo bash /home/player/HellForge/linux/allow-namespace.sh   (docs/linux.md)\n";
            check(SteamOwnershipCheck.LinuxWrapperProblem(start + appArmor) ==
                "This system only lets allowed programs make the network namespace HellForge runs in. Run this once, then start HellForge again:",
                "The AppArmor line was not reported.");

            const string bridge = "Steam's local ports could not be bridged: is Steam running and signed in?\n";
            check(SteamOwnershipCheck.LinuxWrapperProblem(start + bridge) ==
                "Steam's local ports could not be bridged: is Steam running and signed in?", "The bridge line was not reported.");
        });

        test("The Linux ownership check ignores an empty wrapper log", () =>
        {
            check(SteamOwnershipCheck.LinuxWrapperProblem("") == null, "An empty log reported a failure.");
        });

        test("The Linux ownership check reads the wrapper log only when this run wrote it", () =>
        {
            var folder = Directory.CreateDirectory(Path.Combine(root, "ownership-log")).FullName;
            var path = Path.Combine(folder, "linux-session-wrapper.log");
            var checkedAt = new DateTime(2026, 1, 1, 12, 0, 0, DateTimeKind.Utc);
            File.WriteAllText(path, start + "Steam's local ports could not be bridged: is Steam running and signed in?\n");
            File.SetLastWriteTimeUtc(path, checkedAt.AddMinutes(-1));
            check(SteamOwnershipCheck.ReadLinuxWrapperProblem(path, checkedAt) == null, "An earlier run's wrapper log was read.");
            File.SetLastWriteTimeUtc(path, checkedAt.AddMinutes(1));
            check(SteamOwnershipCheck.ReadLinuxWrapperProblem(path, checkedAt) ==
                "Steam's local ports could not be bridged: is Steam running and signed in?", "This run's wrapper log was not read.");
            check(SteamOwnershipCheck.ReadLinuxWrapperProblem(Path.Combine(folder, "missing.log"), checkedAt) == null,
                "A missing wrapper log reported a failure.");
        });
    }
}
