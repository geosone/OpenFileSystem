/* SPDX-License-Identifier: GPL-2.0-or-later */
/* see include/ofs/dostime.h */
#include "ofs/dostime.h"

/* days since 1970-01-01 of a date (Howard Hinnant's days_from_civil) */
static ofs_s32 days(ofs_s32 y, unsigned m, unsigned d)
{
    ofs_s32 era, yoe, doy, doe;
    y -= m <= 2;
    era = (y >= 0 ? y : y - 399) / 400;
    yoe = y - era * 400;
    doy = (ofs_s32)((153 * (m + (m > 2 ? (unsigned)-3 : 9)) + 2) / 5) + (ofs_s32)d - 1;
    doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097L + doe - 719468L;
}

static void civil(ofs_s32 z, ofs_s32 *y, unsigned *m, unsigned *d)
{
    ofs_s32 era, doe, yoe, doy, mp;
    z += 719468L;
    era = (z >= 0 ? z : z - 146096L) / 146097L;
    doe = z - era * 146097L;
    yoe = (doe - doe / 1460 + doe / 36524L - doe / 146096L) / 365;
    *y = yoe + era * 400;
    doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    mp = (5 * doy + 2) / 153;
    *d = (unsigned)(doy - (153 * mp + 2) / 5 + 1);
    *m = (unsigned)(mp < 10 ? mp + 3 : mp - 9);
    *y += *m <= 2;
}

void ofs_dostime(ofs_s32 y, unsigned mo, unsigned d, unsigned h, unsigned mi, unsigned s,
                 ofs_s32 zone, ofs_s32 tz, ofs_u16 *time, ofs_u16 *date)
{
    ofs_s32 min;
    *time = *date = 0;
    if (!mo || mo > 12 || !d || d > 31 || h > 23 || mi > 59 || y < 1970 || y > 2200) return;
    min = (days(y, mo, d) * 24 + (ofs_s32)h) * 60 + (ofs_s32)mi;
    min += tz - zone;
    if (min < 0) return;
    civil(min / 1440, &y, &mo, &d);
    if (y < 1980) return;
    if (y > 2107) { y = 2107; mo = 12; d = 31; min = 1439; s = 59; }
    h = (unsigned)(min % 1440 / 60);
    mi = (unsigned)(min % 60);
    *time = (ofs_u16)(h << 11 | mi << 5 | (s > 59 ? 29 : s / 2));
    *date = (ofs_u16)((ofs_u16)(y - 1980) << 9 | mo << 5 | d);
}
