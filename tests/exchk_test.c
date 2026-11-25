/* The exFAT check on the host, as EXCHK does it under DOS:
   exchk_test IMAGE [LBA [WINDOW [F]]]
   WINDOW: clusters per pass (0: all); F: repair (EXCHK /F); exit 0 clean,
   1 errors, 2 not exFAT or unreadable.  SECTOR=4096 (512 to 4096) in the
   environment: sectors of that size. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "testcp.h"
#include "ofs/exfat_check.h"

static FILE *image;
static unsigned char *bits;
static long bps = 512;

static int img_read(void *dev, ofs_lba lba, void *buf)
{
    (void)dev;
    if (fseek(image, (long)lba * bps, SEEK_SET)) return -1;
    return fread(buf, (size_t)bps, 1, image) == 1 ? 0 : -1;
}

static int img_write(void *dev, ofs_lba lba, const void *buf)
{
    (void)dev;
    if (fseek(image, (long)lba * bps, SEEK_SET)) return -1;
    return fwrite(buf, (size_t)bps, 1, image) == 1 ? 0 : -1;
}

static void msg(void *ctx, int level, const char *text)
{
    (void)ctx;
    printf("%s%s\n", level == OFS_EXCHK_ERROR ? "ERROR: " : level == OFS_EXCHK_WARN ? "WARNING: " :
           level == OFS_EXCHK_FIXED ? "  FIXED: " : "", text);
}

static int mark(void *ctx, ofs_u32 bit)
{
    int old = bits[bit >> 3] >> (bit & 7) & 1;
    (void)ctx;
    bits[bit >> 3] |= (unsigned char)(1 << (bit & 7));
    return old;
}

static ofs_u32 window;
static void clear(void *ctx) { (void)ctx; memset(bits, 0, window / 8 + 1); }

int main(int argc, char **argv)
{
    struct ofs_exchk c;
    int r;
    int fix = argc > 4 && argv[4][0] == 'F';
    if (argc < 2) { fputs("exchk_test IMAGE [LBA [WINDOW [F]]]\n", stderr); return 2; }
    image = fopen(argv[1], fix ? "r+b" : "rb");
    if (!image) { perror(argv[1]); return 2; }
    if (getenv("SECTOR")) bps = atol(getenv("SECTOR"));
    memset(&c, 0, sizeof(c));
    c.read = img_read; c.cp = test_cp(850); c.msg = msg; c.mark = mark; c.clear = clear;
    c.start = argc > 2 ? strtoul(argv[2], NULL, 0) : 0;
    window = argc > 3 && strtoul(argv[3], NULL, 0) ? strtoul(argv[3], NULL, 0) : 1UL << 28;
    if (fix) c.write = img_write;
    c.window = window;
    bits = malloc(window / 8 + 1);
    r = ofs_exchk_run(&c);
    if (r) { printf("not checked: %s\n", r == OFS_EXFAT_EIO ? "read error" : "no exFAT"); return 2; }
    printf("%lu files, %lu directories; %lu clusters of %lu bytes, %lu used, %lu free\n",
           (unsigned long)c.files, (unsigned long)c.dirs, (unsigned long)c.clusters,
           (unsigned long)c.cluster_size, (unsigned long)c.used, (unsigned long)c.free);
    printf("%lu errors, %lu warnings, %lu fixed; lost %lu, unmarked %lu, cross-linked %lu, bad %lu\n",
           (unsigned long)c.errors, (unsigned long)c.warnings, (unsigned long)c.fixed, (unsigned long)c.lost,
           (unsigned long)c.unmarked, (unsigned long)c.cross, (unsigned long)c.bad);
    return c.errors ? 1 : 0;
}
