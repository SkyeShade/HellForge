using System.Text;
using HellForge.Core;
using HellForge.Launcher;

// MainWindow's session poll: result.json is parsed again only after it changed, and a run with completedUtc whose runner
// has exited is not polled; LineTail reads requests.jsonl in chunks without losing a line or splitting a UTF-8 character.
static class LauncherPollTests
{
    static string ResultPath(string root, string name)
    {
        var folder = Directory.CreateDirectory(Path.Combine(root, "launcher-poll", name)).FullName;
        return Path.Combine(folder, "result.json");
    }

    public static void Register(Action<string, Action> test, Action<bool, string> check, string root)
    {
        test("result.json: an unchanged file is parsed once", () =>
        {
            var path = ResultPath(root, "unchanged");
            System.IO.File.WriteAllText(path, """{"status": "preparing"}""");
            var parses = 0;
            var result = new ResultFile(p => { parses++; return SessionFiles.ReadJson(p); });
            var first = result.Read(path, true);
            var second = result.Read(path, true);
            check(parses == 1, $"An unchanged result.json was parsed {parses} times.");
            check(first is { } a && second is { } b && a.GetRawText() == b.GetRawText(), "The cached state was not returned.");
        });

        test("result.json: a changed file is parsed again", () =>
        {
            var path = ResultPath(root, "changed");
            System.IO.File.WriteAllText(path, """{"status": "preparing"}""");
            var parses = 0;
            var result = new ResultFile(p => { parses++; return SessionFiles.ReadJson(p); });
            var first = result.Read(path, true);
            System.IO.File.WriteAllText(path, """{"status": "observed"}""");
            var second = result.Read(path, true);
            check(parses == 2, $"A changed result.json was parsed {parses} times.");
            check(first is { } a && SessionFiles.Text(a, "status") == "preparing" &&
                second is { } b && SessionFiles.Text(b, "status") == "observed", "The new state was not read.");
        });

        test("result.json: a finished run stops being polled", () =>
        {
            var path = ResultPath(root, "finished");
            System.IO.File.WriteAllText(path, """{"status": "observed", "completedUtc": "2026-10-28T10:00:00Z"}""");
            var parses = 0;
            var result = new ResultFile(p => { parses++; return SessionFiles.ReadJson(p); });
            check(result.Read(path, true) is not null && !result.Finished && parses == 1,
                "A completed result is final while the runner still runs.");
            check(result.Read(path, false) is not null && result.Finished && parses == 1,
                "The last state was not shown and cached when the runner exited.");
            System.IO.File.WriteAllText(path, """{"status": "failed", "completedUtc": "2026-10-28T10:00:01Z", "late": true}""");
            check(result.Read(path, false) is null && parses == 1, "A finished run was read again.");
        });

        test("result.json: a runner that ends without completedUtc is final, not still running", () =>
        {
            var path = ResultPath(root, "ended-without-cleanup");
            System.IO.File.WriteAllText(path, """{"status": "patching"}""");
            var result = new ResultFile(p => SessionFiles.ReadJson(p));
            check(result.Read(path, true) is not null && !result.Finished, "A live run was treated as ended.");
            check(result.Read(path, false) is not null && result.Finished, "A runner that ended without completedUtc stayed live.");
            check(result.Read(path, false) is null, "The ended run was read again.");
        });

        test("LineTail: the same lines across chunk boundaries, with a UTF-8 character split between reads", () =>
        {
            var path = ResultPath(root, "line-tail-chunks");
            const string first = "{\"first\":true}\n";
            var filler = new string('a', LineTail.ChunkSize - first.Length - 1);
            System.IO.File.WriteAllText(path, first + filler + "€tail\nlast\n", new UTF8Encoding(false));
            var tail = new LineTail();
            var lines = tail.ReadNewLines(path);
            check(lines.SequenceEqual([first.TrimEnd('\n')]), "The first poll did not stop at its chunk: " + string.Join(" | ", lines));
            lines.AddRange(tail.ReadNewLines(path));
            check(lines.SequenceEqual([first.TrimEnd('\n'), filler + "€tail", "last"]), "Lines across the chunk boundary changed.");
            check(!lines.Any(line => line.Contains('\uFFFD')), "A UTF-8 character was split at the chunk boundary.");
            check(tail.ReadNewLines(path).Count == 0 && tail.AtEnd, "The end of the file was not kept.");
        });

        test("LineTail: a partial last line is kept for the next poll", () =>
        {
            var path = ResultPath(root, "line-tail-partial");
            System.IO.File.WriteAllText(path, "one\r\n第二", new UTF8Encoding(false));
            var tail = new LineTail();
            check(tail.ReadNewLines(path).SequenceEqual(["one"]), "A partial line was returned with the complete one.");
            System.IO.File.AppendAllText(path, "行 two", new UTF8Encoding(false));
            check(tail.ReadNewLines(path).Count == 0, "An unfinished line was returned.");
            System.IO.File.AppendAllText(path, "!\nthree", new UTF8Encoding(false));
            check(tail.ReadNewLines(path).SequenceEqual(["第二行 two!"]), "The line split across polls was not kept.");
            check(tail.ReadNewLines(path).Count == 0, "A partial line was returned at the end.");
            System.IO.File.AppendAllText(path, "\n", new UTF8Encoding(false));
            check(tail.ReadNewLines(path).SequenceEqual(["three"]), "The last line was not returned once its newline arrived.");
        });
    }
}
