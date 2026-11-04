// B.5: the crash dump writer keeps 2 GB free: a full-memory dump becomes the small dump when it would leave less.
static class CrashDumpTests
{
    public static void Register(Action<string, Action> test, Action<bool, string> check)
    {
        test("A crash dump falls back to the small dump when a full one would leave less than 2 GB", () =>
        {
            check(CrashDump.Reserve == 2L << 30, "The reserve changed from 2 GB.");
            check(!CrashDump.SmallDump(null, 64L << 30), "An unreadable file system did not keep the full dump.");
            check(!CrashDump.SmallDump(CrashDump.Reserve, 0), "Exactly 2 GB left was treated as less than 2 GB.");
            check(CrashDump.SmallDump(CrashDump.Reserve - 1, 0), "Less than 2 GB left kept the full dump.");
            check(CrashDump.SmallDump(5L << 30, 4L << 30), "A full dump leaving 1 GB did not choose the small dump.");
            check(!CrashDump.SmallDump(7L << 30, 4L << 30), "A full dump leaving 3 GB chose the small dump.");
            check(CrashDump.FullDumpBytes(3L << 30, 5L << 30) == 5L << 30 &&
                CrashDump.FullDumpBytes(7L << 30, 5L << 30) == 7L << 30,
                "The full dump's estimate did not take the larger of private bytes and working set.");
        });
    }
}
