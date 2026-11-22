/* A field's range with its default in it (catalog.h Row::RangeWith): a default under the minimum or over the maximum
 * widens that end, one inside changes nothing, an open end stays open, and no default or NaN leaves the catalogue's.
 * Usage: range_test.exe */
#include <math.h>
#include <stdio.h>
#include "../catalog.h"

static int g_failures;
static void Check(bool ok, const char* what) { printf("%s %s\n", ok ? "  ok  " : "  FAIL", what); if (!ok) g_failures++; }

int main()
{
    catalog::Row radius;
    radius.hasMin = radius.hasMax = true;
    radius.min = 0.1;
    radius.max = 100;
    catalog::Row::Range r = radius.RangeWith(true, 0);
    Check(r.hasMin && r.min == 0 && r.hasMax && r.max == 100, "a default 0 under 0.1 to 100 widens the minimum to 0");
    r = radius.RangeWith(true, 5);
    Check(r.min == 0.1 && r.max == 100, "a default inside the range changes nothing");
    r = radius.RangeWith(true, 3.4e38);
    Check(r.min == 0.1 && r.max == 3.4e38, "an unset 3.4e38 default widens the maximum");
    r = radius.RangeWith(false, 0);
    Check(r.min == 0.1, "no default: the catalogue's range");
    r = radius.RangeWith(true, NAN);
    Check(r.min == 0.1 && r.max == 100, "a NaN default: the catalogue's range");
    catalog::Row open;
    open.hasMin = true;
    r = open.RangeWith(true, -1);
    Check(r.hasMin && r.min == -1 && !r.hasMax, "a sentry's unset -1 under 0 widens the minimum; an open maximum stays open");
    printf(g_failures ? "%d failed\n" : "all passed\n", g_failures);
    return g_failures ? 1 : 0;
}
