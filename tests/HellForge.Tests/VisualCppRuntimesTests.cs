using System.ComponentModel;
using System.Net;
using System.Security.Cryptography;
using HellForge.Core;

// The Microsoft Visual C++ runtimes the game needs (VisualCppRuntimes): what counts as missing, and the automatic install
// setup and PLAY run before the message naming the download.
static class VisualCppRuntimesTests
{
    sealed class Respond(Func<HttpRequestMessage, HttpResponseMessage> answer) : HttpMessageHandler
    {
        protected override Task<HttpResponseMessage> SendAsync(HttpRequestMessage request, CancellationToken cancellationToken) =>
            Task.FromResult(answer(request));
    }

    // The smallest file IsX64Loadable reads as a PE image for the given machine (0x8664 x64, 0x14C x86).
    static byte[] Image(ushort machine)
    {
        var bytes = new byte[0x100];
        bytes[0] = (byte)'M';
        bytes[1] = (byte)'Z';
        BitConverter.GetBytes(0x80).CopyTo(bytes, 0x3C);
        "PE\0\0"u8.ToArray().CopyTo(bytes, 0x80);
        BitConverter.GetBytes(machine).CopyTo(bytes, 0x84);
        return bytes;
    }

    static void Put(string folder, string file, ushort machine = 0x8664) => File.WriteAllBytes(Path.Combine(folder, file), Image(machine));

    public static void Register(Action<string, Action> test, Action<bool, string> check, Action<Action> reject, string root)
    {
        test("A PC without the Visual C++ 2012 or 2015-2022 x64 runtime is told which to install before the game starts", () =>
        {
            var system = Directory.CreateDirectory(Path.Combine(root, "vc-runtimes")).FullName;
            var none = VisualCppRuntimes.Problem(system, "press Play again.");
            check(none != null && none.Contains("2012") && none.Contains("2015-2022") && none.EndsWith("press Play again."), "Both missing: " + none);
            Put(system, "msvcr110.dll");
            Put(system, "vcruntime140.dll");
            Put(system, "msvcp140.dll");
            var partial = VisualCppRuntimes.Problem(system, "start setup again.");
            check(partial != null && !partial.Contains("2012") && partial.Contains("2015-2022") && partial.Contains("a Microsoft runtime"),
                "vcruntime140_1.dll (XeSS, EOS, the upscaler) missing: " + partial);
            Put(system, "vcruntime140_1.dll");
            check(VisualCppRuntimes.Problem(system, "start setup again.") == null, "Every runtime is there.");
            check(File.Exists(Path.Combine(VisualCppRuntimes.SystemFolder, "kernel32.dll")), "Not the 64-bit system folder: " + VisualCppRuntimes.SystemFolder);
        });

        test("A runtime DLL that is 32-bit, damaged or empty in System32 counts as missing", () =>
        {
            var system = Directory.CreateDirectory(Path.Combine(root, "vc-runtimes-bad")).FullName;
            foreach (var file in new[] { "vcruntime140.dll", "vcruntime140_1.dll", "msvcp140.dll" }) Put(system, file);
            Put(system, "msvcr110.dll", 0x14C);
            check(VisualCppRuntimes.Missing(system).Select(r => r.Files[0]).SequenceEqual(["msvcr110.dll"]), "A 32-bit msvcr110.dll (from a DLL download site).");
            File.WriteAllText(Path.Combine(system, "msvcr110.dll"), "synthetic");
            check(VisualCppRuntimes.Missing(system).Count == 1, "Not a PE image.");
            File.WriteAllBytes(Path.Combine(system, "msvcr110.dll"), []);
            check(VisualCppRuntimes.Missing(system).Count == 1, "Empty.");
            Put(system, "msvcr110.dll");
            check(VisualCppRuntimes.Missing(system).Count == 0, "x64.");
        });

        test("The runtime installers are pinned: Microsoft's HTTPS downloads, each with its size and SHA-256", () =>
        {
            foreach (var runtime in VisualCppRuntimes.Required)
                check(runtime.Download.StartsWith("https://download.", StringComparison.Ordinal) && new Uri(runtime.Download).Host.EndsWith(".microsoft.com", StringComparison.Ordinal) &&
                    runtime.Sha256.Length == 64 && runtime.Sha256.All(Uri.IsHexDigit) && runtime.Size > 1_000_000 && runtime.Link.StartsWith("https://", StringComparison.Ordinal), runtime.Name);
        });

        test("Installer exit codes: restart wanted and already installed pass; cancelled, busy and others say why", () =>
        {
            foreach (var code in new[] { 0, 3010, 1641, 1638 }) check(VisualCppRuntimes.ExitCodeProblem(code) == null, code.ToString());
            check(VisualCppRuntimes.ExitCodeProblem(1602)!.Contains("cancelled"), "1602");
            check(VisualCppRuntimes.ExitCodeProblem(1618)!.Contains("another installation"), "1618");
            check(VisualCppRuntimes.ExitCodeProblem(1223)!.Contains("permission prompt"), "1223");
            check(VisualCppRuntimes.ExitCodeProblem(1603)!.Contains("1603"), "1603");
        });

        // A runtime pinned to a synthetic installer, served by a fake Microsoft.
        var installer = "synthetic installer"u8.ToArray();
        var runtime = new VisualCppRuntimes.Runtime("Microsoft Visual C++ 2012 Redistributable (x64)", ["msvcr110.dll"],
            "https://download.microsoft.com/vcredist_x64.exe", Convert.ToHexString(SHA256.HashData(installer)), installer.Length,
            "https://aka.ms/vc-lasting-link");
        var downloads = 0;
        // a null body: the pinned build is gone (404)
        HttpClient Microsoft(byte[]? body) => new(new Respond(_ =>
        {
            downloads++;
            return body == null ? new HttpResponseMessage(HttpStatusCode.NotFound) : new HttpResponseMessage(HttpStatusCode.OK) { Content = new ByteArrayContent(body) };
        }));
        string? Ensure(string name, byte[]? body, VisualCppRuntimes.InstallerRunner run, List<string>? said = null)
        {
            var folder = Directory.CreateDirectory(Path.Combine(root, "vc-install", name)).FullName;
            using var http = Microsoft(body);
            var at = new VisualCppRuntimes.Installation(Directory.CreateDirectory(Path.Combine(folder, "system")).FullName, Path.Combine(folder, "temp"),
                http, run, line => said?.Add(line), [runtime]);
            return VisualCppRuntimes.EnsureAsync(at, "press PLAY again.").GetAwaiter().GetResult();
        }
        string System(string name) => Path.Combine(root, "vc-install", name, "system");

        test("A missing runtime is downloaded, checked and installed silently, then nothing is left to tell the player", () =>
        {
            var calls = new List<string>();
            var said = new List<string>();
            var result = Ensure("installs", installer, (file, arguments, _) =>
            {
                calls.Add(arguments);
                check(File.ReadAllBytes(file).SequenceEqual(installer), "The checked installer runs.");
                if (OperatingSystem.IsWindows()) reject(() => File.WriteAllText(file, "swapped"));  // Windows share modes; Unix locks are advisory
                Put(System("installs"), "msvcr110.dll");
                return Task.FromResult(3010);
            }, said);
            check(result == null, "Installed: " + result);
            check(calls.Count == 1 && calls[0].StartsWith("/install /quiet /norestart /log ", StringComparison.Ordinal), string.Join(" | ", calls));
            check(said.Any(line => line.StartsWith("Downloading")) && said.Any(line => line.Contains("Windows asks for permission")), string.Join(" | ", said));
        });

        test("A runtime listed as installed whose DLL is gone is repaired", () =>
        {
            var calls = new List<string>();
            var result = Ensure("repairs", installer, (_, arguments, _) =>
            {
                calls.Add(arguments);
                if (arguments.StartsWith("/repair", StringComparison.Ordinal)) Put(System("repairs"), "msvcr110.dll");
                return Task.FromResult(arguments.StartsWith("/install", StringComparison.Ordinal) ? 1638 : 0);
            });
            check(result == null && calls.Count == 2 && calls[1].StartsWith("/repair /quiet /norestart", StringComparison.Ordinal), result + " " + string.Join(" | ", calls));
        });

        test("When the automatic install fails, the player is told why and given Microsoft's download", () =>
        {
            var declined = Ensure("declined", installer, (_, _, _) => throw new Win32Exception(1223));
            check(declined != null && declined.Contains("permission prompt was declined") && declined.Contains(runtime.Link) && !declined.Contains(runtime.Download) && declined.EndsWith("press PLAY again."), "Declined: " + declined);
            var ran = false;
            var tampered = Ensure("tampered", "not the installer"u8.ToArray(), (_, _, _) => { ran = true; return Task.FromResult(0); });
            check(!ran && tampered != null && tampered.Contains("pinned checksum") && tampered.Contains(runtime.Link), "A download that does not match: " + tampered);
            check(!File.Exists(Path.Combine(root, "vc-install", "tampered", "temp", runtime.Sha256[..8] + "-vcredist_x64.exe")), "The mismatched download is not kept.");
            var busy = Ensure("busy", installer, (_, _, _) => Task.FromResult(1618));
            check(busy != null && busy.Contains("another installation") && busy.Contains("installer log"), "Busy: " + busy);
            var nothing = Ensure("nothing", installer, (_, _, _) => Task.FromResult(0));
            check(nothing != null && nothing.Contains("still not in"), "Installer claimed success: " + nothing);
            var gone = Ensure("gone", null, (_, _, _) => { ran = true; return Task.FromResult(0); });
            check(!ran && gone != null && gone.Contains("download from Microsoft failed") && gone.Contains("404") && gone.Contains(runtime.Link),
                "The pinned build retired: " + gone);
        });

        test("An installer already downloaded with its pinned checksum is not downloaded again", () =>
        {
            downloads = 0;
            VisualCppRuntimes.InstallerRunner none = (_, _, _) => Task.FromResult(0);
            Ensure("cached", installer, none);
            Ensure("cached", installer, none);
            check(downloads == 1, "Downloads: " + downloads);
        });
    }
}
