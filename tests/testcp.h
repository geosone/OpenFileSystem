/* The code pages of the tests: UNI_<cp>.TAB of mars-unicode-tables
   (UNITAB, the directory of the files, set by the Makefile). */
#ifndef TESTCP_H
#define TESTCP_H
#include <stdio.h>
#include <stdlib.h>
#include "ofs/codepage.h"

static const struct ofs_cp *test_cp(unsigned number)
{
    static struct ofs_cp cps[4];
    static unsigned n;
    static ofs_u8 tab[8192];
    char path[512];
    unsigned i;
    size_t len;
    FILE *f;
    for (i = 0; i < n; ++i) if (cps[i].cp == number) return &cps[i];
    sprintf(path, "%s/UNI_%03u.TAB", UNITAB, number);
    if (!(f = fopen(path, "rb"))) { fprintf(stderr, "%s: cannot open (UNITAB)\n", path); exit(1); }
    len = fread(tab, 1, sizeof(tab), f);
    fclose(f);
    if (n == 4 || ofs_cp_load(&cps[n], (ofs_u16)number, tab, (ofs_u32)len)) {
        fprintf(stderr, "%s: no single-byte code page\n", path);
        exit(1);
    }
    return &cps[n++];
}
#endif
