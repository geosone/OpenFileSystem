/* FAT recovery (ORECOVER) on the host:
   recover_test IMAGE OUTDIR [FLAGS [BAD]]
   FLAGS: S scan for lost directories, C lost chains (e.g. SC; - none).
   BAD: sectors that cannot be read, "a,b,c" or "a-b".
   The files go below OUTDIR (which must exist); a name that is there
   already gets ~2, ~3 ...  Prints the counts. */
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include "ofs/fat_recover.h"

#define MEM (16UL << 20)

static FILE *image, *outf;
static unsigned char *mem;
static const char *outdir;
static char bad[256];
static unsigned long badlo[64], badhi[64];
static unsigned nbad;

static int img_read(void *dev, ofs_lba lba, void *buf)
{
    unsigned i;
    (void)dev;
    for (i = 0; i < nbad; ++i)
        if (lba >= badlo[i] && lba <= badhi[i]) return -1;
    if (fseek(image, (long)lba * 512, SEEK_SET)) return -1;
    return fread(buf, 512, 1, image) == 1 ? 0 : -1;
}

static void *at(void *ctx, ofs_u32 off) { (void)ctx; return mem + off; }

/* OUTDIR plus the path, \ as / */
static void host_path(const char *p, char *out, size_t max)
{
    size_t n = (size_t)snprintf(out, max, "%s/", outdir), i;
    for (i = 0; p[i] && n + 1 < max; ++i) out[n++] = p[i] == '\\' ? '/' : p[i];
    out[n] = 0;
}

static int t_mkdir(void *ctx, const char *p)
{
    char h[2048];
    (void)ctx;
    host_path(p, h, sizeof(h));
    return mkdir(h, 0755) && errno != EEXIST ? -1 : 0;
}

static int t_create(void *ctx, const char *p, ofs_fsize size, ofs_u16 time, ofs_u16 date)
{
    char h[2048], h2[2100];
    struct stat st;
    int k;
    (void)ctx, (void)size, (void)time, (void)date;
    host_path(p, h, sizeof(h));
    strcpy(h2, h);
    for (k = 2; !stat(h2, &st); ++k) sprintf(h2, "%s~%d", h, k);
    outf = fopen(h2, "wb");
    return outf ? 0 : -1;
}

static int t_write(void *ctx, const void *buf, unsigned len)
{
    (void)ctx;
    return fwrite(buf, 1, len, outf) == len ? 0 : -1;
}

static int t_close(void *ctx)
{
    (void)ctx;
    return fclose(outf) ? -1 : 0;
}

static void msg(void *ctx, int level, const char *text)
{
    (void)ctx;
    printf("%s%s\n", level == OFS_FATREC_ERROR ? "ERROR: " : level == OFS_FATREC_WARN ? "WARNING: " : "", text);
}

int main(int argc, char **argv)
{
    static ofs_u8 buf[2 * OFS_FATREC_MAXSECT];
    struct ofs_fatrec r;
    const char *f = argc > 3 ? argv[3] : "SC";
    char *p;
    int rc;
    if (argc < 3) { fputs("recover_test IMAGE OUTDIR [FLAGS [BAD]]\n", stderr); return 2; }
    if (!(image = fopen(argv[1], "rb"))) { perror(argv[1]); return 2; }
    outdir = argv[2];
    if (argc > 4) {
        strncpy(bad, argv[4], sizeof(bad) - 1);
        for (p = strtok(bad, ","); p && nbad < 64; p = strtok(NULL, ",")) {
            badlo[nbad] = strtoul(p, &p, 0);
            badhi[nbad] = *p == '-' ? strtoul(p + 1, NULL, 0) : badlo[nbad];
            ++nbad;
        }
    }
    mem = malloc(MEM);
    memset(&r, 0, sizeof(r));
    r.read = img_read;
    r.flags = (strchr(f, 'S') ? OFS_FATREC_SCAN : 0) | (strchr(f, 'C') ? OFS_FATREC_CHAINS : 0);
    r.mkdir = t_mkdir, r.create = t_create, r.write = t_write, r.close = t_close;
    r.msg = msg;
    r.mem = at, r.memsize = MEM, r.block = 1UL << 30;
    r.buf = buf;
    {
        unsigned char b[512];
        if (fread(b, 1, sizeof(b), image) == sizeof(b) && !memcmp(b + 3, "EXFAT   ", 8)) rc = ofs_exfatrec_run(&r);
        else if (fseek(image, 12L * 512, SEEK_SET) == 0 && fread(b, 1, sizeof(b), image) == sizeof(b) &&
                 !memcmp(b + 3, "EXFAT   ", 8)) rc = ofs_exfatrec_run(&r);
        else rc = ofs_fatrec_run(&r);
    }
    if (rc) { printf("not recovered: %d\n", rc); return 2; }
    if (r.bits == 64) printf("exFAT%s", r.texfat ? " (TexFAT)" : ""); else printf("FAT%u", r.bits);
    printf(": %lu files, %lu directories, %lu bytes; guessed %lu, bad sectors %lu, lost directories %lu, "
           "lost chains %lu, failed %lu\n", (unsigned long)r.files, (unsigned long)r.dirs,
           (unsigned long)r.bytes_lo, (unsigned long)r.guessed, (unsigned long)r.bad, (unsigned long)r.lost_dirs,
           (unsigned long)r.lost_chains, (unsigned long)r.failed);
    return 0;
}
