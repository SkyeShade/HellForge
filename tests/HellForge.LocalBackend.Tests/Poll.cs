// Waits for what another thread does (a frame the in-memory network delivers, a backend's event): true once the condition
// holds, false if it still does not after the time. A fixed sleep before a check fails when a slow or busy runner is late.
static class Poll
{
    public static bool Until(Func<bool> condition, double seconds = 5)
    {
        var deadline = DateTime.UtcNow.AddSeconds(seconds);
        while (!condition())
        {
            if (DateTime.UtcNow >= deadline) return false;
            Thread.Sleep(10);
        }
        return true;
    }
}
