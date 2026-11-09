using System.Globalization;

namespace HellForge.Core;

// A log's dated line, written each time the file is opened, as native/common/hf_log_header.h writes it for the native
// logs: their lines carry only a time of day, so this one says the local date and time and the offset from UTC.
//     log opened 2026-10-26 05:34:56 local time (UTC-07:00); the times below are UTC
public static class LogHeader
{
    public static string Line(DateTimeOffset opened, bool utcTimes)
    {
        var offset = opened.Offset;
        var sign = offset < TimeSpan.Zero ? "-" : "+";
        var whole = offset.Duration();
        return string.Create(CultureInfo.InvariantCulture,
            $"log opened {opened:yyyy-MM-dd HH:mm:ss} local time (UTC{sign}{whole.Hours:00}:{whole.Minutes:00}); the times below are {(utcTimes ? "UTC" : "local")}");
    }
}
