/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef OFS_DOSTIME_H
#define OFS_DOSTIME_H
/* Times of the cores whose file systems keep a calendar time with a zone
   (ISO 9660, UDF) as the local DOS form of the host. */
#include "ofs/types.h"

/* y-mo-d h:mi:s at `zone` minutes east of UTC, as a DOS time of the host
   at `tz` minutes east of UTC; 0/0 when out of range (before 1980, a bad
   field), after 2107 the last DOS day. */
void ofs_dostime(ofs_s32 y, unsigned mo, unsigned d, unsigned h, unsigned mi, unsigned s,
                 ofs_s32 zone, ofs_s32 tz, ofs_u16 *time, ofs_u16 *date);
#endif
