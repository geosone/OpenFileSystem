/* SPDX-License-Identifier: GPL-2.0-or-later */
/* src/lnk.c: shortcuts as symlinks, Cygwin's form */
#include <stdio.h>
#include <string.h>
#include "ofs/lnk.h"

static int failed;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); failed++; } } while (0)

static unsigned u16(const char *s, ofs_u16 *out)
{
    unsigned n = 0;
    while (*s) out[n++] = (unsigned char)*s++;
    return n;
}

int main(void)
{
    static ofs_u8 buf[OFS_LNK_FILE_MAX];
    ofs_u16 t[OFS_LNK_TARGET_MAX + 1], got[OFS_LNK_TARGET_MAX];
    unsigned n, glen, i;
    ofs_u32 len;

    /* a relative target round trip; the parts as Cygwin writes them */
    n = u16("../dir/file.txt", t);
    len = ofs_lnk_make(t, n, buf, sizeof(buf));
    CHECK(len == 0x4C + 2 + n + 2 + n + 4 + 2 * n);
    CHECK(buf[0] == 0x4C && buf[0x14] == 0x0C && buf[0x3C] == 1);
    CHECK(!memcmp(buf + 0x4E, "../dir/file.txt", n));                       /* the description */
    CHECK(!memcmp(buf + 0x4E + n + 2, "..\\dir\\file.txt", n));             /* the Win32 path */
    CHECK(ofs_lnk_read(buf, len, got, OFS_LNK_TARGET_MAX, &glen) && glen == n && !memcmp(got, t, 2 * n));

    /* beyond ASCII: the UTF-16 path is the one read */
    n = u16("Gr", t);
    t[n++] = 0x00F6;                                                         /* o umlaut */
    t[n++] = 0x03A9;                                                         /* Omega */
    len = ofs_lnk_make(t, n, buf, sizeof(buf));
    CHECK(len && ofs_lnk_read(buf, len, got, OFS_LNK_TARGET_MAX, &glen) && glen == n && got[3] == 0x03A9);
    CHECK(buf[0x4E + 2] == '?');                                             /* ANSI: lossy */

    /* longer than MAX_PATH: "[path too long]" as description */
    for (i = 0; i < 300; ++i) t[i] = 'a';
    len = ofs_lnk_make(t, 300, buf, sizeof(buf));
    CHECK(len && buf[0x4C] == 15 && !memcmp(buf + 0x4E, "[path too long]", 15));
    CHECK(ofs_lnk_read(buf, len, got, OFS_LNK_TARGET_MAX, &glen) && glen == 300);
    CHECK(!ofs_lnk_read(buf, len, got, 299, &glen));                         /* too long for the caller */
    CHECK(!ofs_lnk_make(t, OFS_LNK_TARGET_MAX + 1, buf, sizeof(buf)));
    CHECK(!ofs_lnk_make(t, 0, buf, sizeof(buf)));
    CHECK(!ofs_lnk_make(t, 10, buf, 50));

    /* an older Cygwin's: description and relative path only */
    n = u16("/usr/bin", t);
    len = ofs_lnk_make(t, n, buf, sizeof(buf));
    len = 0x4C + 2 + n + 2 + n;
    CHECK(ofs_lnk_read(buf, len, got, OFS_LNK_TARGET_MAX, &glen) && glen == n && got[0] == '/');

    /* not links: a shortcut with a LinkInfo (Explorer's), shown
       minimised, a short file, another header size, a Cygwin device */
    n = u16("x", t);
    len = ofs_lnk_make(t, n, buf, sizeof(buf));
    buf[0x14] |= 0x02;
    CHECK(!ofs_lnk_read(buf, len, got, OFS_LNK_TARGET_MAX, &glen));
    buf[0x14] &= ~0x02;
    buf[0x3C] = 7;
    CHECK(!ofs_lnk_read(buf, len, got, OFS_LNK_TARGET_MAX, &glen));
    buf[0x3C] = 1;
    CHECK(ofs_lnk_read(buf, len, got, OFS_LNK_TARGET_MAX, &glen));
    CHECK(!ofs_lnk_read(buf, 0x4C, got, OFS_LNK_TARGET_MAX, &glen));
    buf[0] = 0x4D;
    CHECK(!ofs_lnk_read(buf, len, got, OFS_LNK_TARGET_MAX, &glen));
    n = u16(":\\dev", t);
    len = ofs_lnk_make(t, n, buf, sizeof(buf));
    CHECK(!ofs_lnk_read(buf, len, got, OFS_LNK_TARGET_MAX, &glen));

    /* with an item ID list in front (Cygwin may write one) */
    n = u16("ziel", t);
    len = ofs_lnk_make(t, n, buf, sizeof(buf) - 8);
    memmove(buf + 0x4C + 6, buf + 0x4C, len - 0x4C);
    buf[0x4C] = 4; buf[0x4D] = 0; buf[0x4E] = 2; buf[0x4F] = 0; buf[0x50] = 0; buf[0x51] = 0;
    buf[0x14] |= 0x01;
    CHECK(ofs_lnk_read(buf, len + 6, got, OFS_LNK_TARGET_MAX, &glen) && glen == n && got[3] == 'l');

    printf("lnk: %s\n", failed ? "FAILED" : "ok");
    return failed != 0;
}
