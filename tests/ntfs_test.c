/* NTFS reading on the host: lists an image made by mkntfs with 8.3
   names, size, attributes and DOS time like DIR, or prints a file.
   ntfs_test                    built-in name tests
   ntfs_test IMAGE [LBA]        list all directories (recursive)
   ntfs_test IMAGE LBA PATH     print a file, PATH in 8.3 names, "\" separated */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "openfat/ntfs.h"
#include "testcp.h"

static FILE *image;
static unsigned long reads;

static int img_read(void *dev, ofat_lba lba, void *buf)
{
    (void)dev; ++reads;
    if (fseek(image, (long)lba * 512, SEEK_SET)) return -1;
    return fread(buf, 512, 1, image) == 1 ? 0 : -1;
}

static struct ofat_ntfs vol;
static ofat_u8 sector_buf[OFAT_NTFS_MAXREC * 3 + 512];

static void name83(const char fcb[11], char *out)
{
    unsigned i, n = 0;
    for (i = 0; i < 8 && fcb[i] != ' '; ++i) out[n++] = fcb[i];
    if (fcb[8] != ' ') { out[n++] = '.'; for (i = 8; i < 11 && fcb[i] != ' '; ++i) out[n++] = fcb[i]; }
    out[n] = 0;
}

static void utf8(const ofat_u16 *s, unsigned n)
{
    unsigned i;
    for (i = 0; i < n; ++i) {
        if (s[i] < 0x80) putchar(s[i]);
        else if (s[i] < 0x800) { putchar(0xC0 | s[i] >> 6); putchar(0x80 | (s[i] & 63)); }
        else { putchar(0xE0 | s[i] >> 12); putchar(0x80 | (s[i] >> 6 & 63)); putchar(0x80 | (s[i] & 63)); }
    }
}

static void list(struct ofat_ntfs_file *dir, const char *path)
{
    static struct ofat_ntfs_ent ent;
    struct ofat_ntfs_file sub;
    ofat_u32 pos = 0, next;
    char fcb[11], name[13], subpath[260];
    int r;
    printf("%s\n", path);
    while (!(r = ofat_ntfs_dir_next(&vol, dir, &pos, &ent))) {
        next = pos;
        assert(!ofat_ntfs_short(&vol, dir, &ent, fcb));
        name83(fcb, name);
        printf("  %-12s %02X %10lu%s %02u.%02u.%04u %02u:%02u  ", name, (unsigned)ent.attr,
               (unsigned long)ent.file.size, ent.file.big ? "+" : " ",
               ent.date & 31, ent.date >> 5 & 15, 1980 + (ent.date >> 9), ent.time >> 11, ent.time >> 5 & 63);
        utf8(ent.name, ent.name_len);
        putchar('\n');
        if (ent.attr & OFAT_NTFS_DIR) {
            sub = ent.file;
            sprintf(subpath, "%s%s\\", path, name);
            list(&sub, subpath);
            pos = next;
        }
    }
    assert(r == OFAT_NTFS_END);
}

static void cat(const char *path)
{
    static struct ofat_ntfs_ent ent;
    struct ofat_ntfs_file dir;
    char fcb[11], part[13];
    static char buf[4096];
    const char *p = path, *e;
    ofat_u32 off = 0;
    ofat_u16 done;
    unsigned i, n;
    int r;
    ofat_ntfs_rootdir(&vol, &dir);
    for (;;) {
        e = strchr(p, '\\');
        n = e ? (unsigned)(e - p) : (unsigned)strlen(p);
        memcpy(part, p, n); part[n] = 0;
        memset(fcb, ' ', 11);
        e = strchr(part, '.');
        for (i = 0; part[i] && part + i != e && i < 8; ++i) fcb[i] = part[i];
        if (e) for (i = 0; e[1+i] && i < 3; ++i) fcb[8+i] = e[1+i];
        if (ofat_ntfs_lookup(&vol, &dir, fcb, &ent)) { fprintf(stderr, "not found: %s\n", part); exit(1); }
        p += n;
        if (!*p) break;
        ++p; dir = ent.file;
    }
    do {
        r = ofat_ntfs_read(&vol, &ent.file, off, buf, sizeof(buf), &done);
        if (r) { fprintf(stderr, "read: %d\n", r); exit(1); }
        fwrite(buf, 1, done, stdout);
        off += done;
    } while (done);
}

int main(int argc, char **argv)
{
            ofat_u32 start = argc > 2 ? strtoul(argv[2], NULL, 0) : 0, free_clusters;
    struct ofat_ntfs_file root;
    int r;
    if (argc < 2) { fprintf(stderr, "usage: ntfs_test IMAGE [LBA [PATH]]\n"); return 2; }
    image = fopen(argv[1], "rb");
    assert(image);
    r = ofat_ntfs_mount(&vol, img_read, NULL, start, test_cp(850), sector_buf);
    if (r) { fprintf(stderr, "mount: %d\n", r); return 1; }
    if (argc > 3) { cat(argv[3]); return 0; }
    assert(!ofat_ntfs_free(&vol, &free_clusters));
    printf("label \""); utf8(vol.label, vol.label_len);
    printf("\" serial %04lX-%04lX, %lu of %lu clusters of %u bytes free%s\n",
           (unsigned long)(vol.serial >> 16), (unsigned long)(vol.serial & 0xFFFF),
           (unsigned long)free_clusters, (unsigned long)vol.clusters, 512u << vol.cshift, vol.dirty ? ", dirty" : "");
    ofat_ntfs_rootdir(&vol, &root);
    list(&root, "\\");
    fprintf(stderr, "%lu sector reads\n", reads);
    return 0;
}
