using System.Net;
using System.Text;
using HellForge.Core;

// The published verified list (VerifiedMods): taken only when signed by the key, never older than the one saved, and read
// again (signature and all) from the installation's saved copy.
static class VerifiedListTests
{
    sealed class Respond(Func<HttpRequestMessage, HttpResponseMessage> answer) : HttpMessageHandler
    {
        protected override Task<HttpResponseMessage> SendAsync(HttpRequestMessage request, CancellationToken cancellationToken) =>
            Task.FromResult(answer(request));
    }

    const string Url = "https://files.example/verified-mods.json";
    static readonly string Other = new('d', 64);

    static byte[] Published(int serial, string hash) => Encoding.UTF8.GetBytes(
        $$"""{"format":1,"serial":{{serial}},"mods":[{"id":"a.b","name":"B","version":"1","hash":"{{hash}}","reviewed":"2026-10-27, maintainer"}]}""");

    public static void Register(Action<string, Action> test, Action<bool, string> check, Action<Action> reject, string root)
    {
        test("Verified list: a published list counts only signed by HellForge's key, with a serial, and never below the one saved", () =>
        {
            var (privateKey, publicKey) = VerifiedMods.NewKey();
            var (otherKey, _) = VerifiedMods.NewKey();
            var two = Published(2, Other);
            var signature = VerifiedMods.Sign(two, privateKey);
            check(VerifiedMods.PublicKeyOf(privateKey) == publicKey && VerifiedMods.Checked(two, signature, publicKey) is { Serial: 2, Mods.Count: 1 },
                "A list signed by the key is taken.");
            var tampered = (byte[])two.Clone();
            tampered[^5] ^= 1;
            reject(() => VerifiedMods.Checked(tampered, signature, publicKey));
            reject(() => VerifiedMods.Checked(two, VerifiedMods.Sign(two, otherKey), publicKey));
            reject(() => VerifiedMods.Checked(two, "not base64!", publicKey));
            var noSerial = Encoding.UTF8.GetBytes($$"""{"format":1,"mods":[]}""");
            reject(() => VerifiedMods.Checked(noSerial, VerifiedMods.Sign(noSerial, privateKey), publicKey));
            reject(() => VerifiedMods.ParseListing("""{"format":1,"serial":-1,"mods":[]}"""));
            check(VerifiedMods.Embedded.Serial >= 1, "The embedded list has a serial.");

            // Fetched, saved and read again by the installation's other processes; offline, the saved copy.
            var install = new InstallLayout(Path.Combine(root, "verified-list"));
            var served = (List: two, Signature: signature);
            var requests = 0;
            HttpClient Http() => new(new Respond(request =>
            {
                requests++;
                var url = request.RequestUri!.AbsoluteUri;
                return url == Url ? new HttpResponseMessage(HttpStatusCode.OK) { Content = new ByteArrayContent(served.List) }
                    : url == Url + VerifiedMods.SignatureSuffix ? new HttpResponseMessage(HttpStatusCode.OK) { Content = new StringContent(served.Signature) }
                    : new HttpResponseMessage(HttpStatusCode.NotFound);
            }));
            (VerifiedMods.Source Source, string? Problem) Refresh(string key) =>
                VerifiedMods.RefreshAsync(Http(), install, TimeSpan.FromSeconds(5), Url, key).GetAwaiter().GetResult();
            try
            {
                var fetched = Refresh(publicKey);
                check(fetched.Source == VerifiedMods.Source.Published && fetched.Problem == null && VerifiedMods.Contains("a.b", Other) &&
                    File.Exists(VerifiedMods.SavedPath(install)), "Fetched, the published list is used and saved: " + fetched.Problem);
                check(VerifiedMods.UseInstallation(install, publicKey) == VerifiedMods.Source.Saved && VerifiedMods.Current.Serial == 2, "Another process reads the saved copy.");
                check(VerifiedMods.UseInstallation(install, VerifiedMods.NewKey().PublicKey) == VerifiedMods.Source.Embedded && !VerifiedMods.Contains("a.b", Other),
                    "Read with another key, the saved copy is not taken.");

                // An older signed list (a mod taken off since) is not taken over the saved one.
                served = (Published(1, new string('e', 64)), VerifiedMods.Sign(Published(1, new string('e', 64)), privateKey));
                var older = Refresh(publicKey);
                check(older.Source == VerifiedMods.Source.Saved && older.Problem!.Contains("serial") && VerifiedMods.Contains("a.b", Other), "An older list is refused: " + older.Problem);
                // A newer one replaces it, taking a mod off.
                var three = Published(3, new string('f', 64));
                served = (three, VerifiedMods.Sign(three, privateKey));
                check(Refresh(publicKey).Source == VerifiedMods.Source.Published && !VerifiedMods.Contains("a.b", Other) && VerifiedMods.Contains("a.b", new string('f', 64)),
                    "A newer list takes a mod off.");
                // Signed by another key, or unreachable: the saved copy stays.
                served = (Published(4, Other), VerifiedMods.Sign(Published(4, Other), otherKey));
                var forged = Refresh(publicKey);
                check(forged.Source == VerifiedMods.Source.Saved && forged.Problem!.Contains("not signed") && VerifiedMods.Current.Serial == 3, "A forged list is refused: " + forged.Problem);
                var offline = VerifiedMods.RefreshAsync(new HttpClient(new Respond(_ => throw new HttpRequestException("offline"))), install, TimeSpan.FromSeconds(5), Url, publicKey)
                    .GetAwaiter().GetResult();
                check(offline.Source == VerifiedMods.Source.Saved && VerifiedMods.Current.Serial == 3, "Offline, the saved copy.");

                // A saved copy changed on disk is not taken.
                var savedFile = VerifiedMods.SavedPath(install);
                File.WriteAllText(savedFile, File.ReadAllText(savedFile).Replace(Convert.ToBase64String(three),
                    Convert.ToBase64String(Published(3, Other))));
                check(VerifiedMods.UseInstallation(install, publicKey) == VerifiedMods.Source.Embedded && !VerifiedMods.Contains("a.b", Other), "An edited saved copy is not taken.");

                // Without a key in this HellForge, nothing is fetched or taken.
                requests = 0;
                check(Refresh("").Source == VerifiedMods.Source.Embedded && requests == 0, "No key: the embedded list, nothing fetched.");
            }
            finally { VerifiedMods.UseInstallation(install, ""); }
            check(VerifiedMods.Current == VerifiedMods.Embedded, "Back to the embedded list.");
        });
    }
}
