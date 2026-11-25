/* NTFS reading on the host: lists an image made by mkntfs with 8.3
   names, size, attributes and DOS time like DIR, or prints a file.
   ntfs_test                    built-in name tests
   ntfs_test IMAGE [LBA]        list all directories (recursive)
   ntfs_test IMAGE LBA PATH     print a file, PATH in 8.3 names, "\" separated
   ntfs_test IMAGE LBA --dump   every directory and file by its long name
                                ("D path", "F path size crc32", "C path" for
                                compressed or encrypted data); checks that the
                                8.3 names of a directory differ (but for hard
                                links of one file) and that
                                ofs_ntfs_lookup finds each entry by its own */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ofs/ntfs.h"
#include "testcp.h"

static FILE *image;
static unsigned long reads;

static unsigned sshift = 9;

static int img_read(void *dev, ofs_lba lba, void *buf)
{
    (void)dev; ++reads;
    if (fseek(image, (long)lba << sshift, SEEK_SET)) return -1;
    return fread(buf, 1u << sshift, 1, image) == 1 ? 0 : -1;
}

static struct ofs_ntfs vol;
static ofs_u8 sector_buf[OFS_NTFS_BUF];

static void name83(const char fcb[11], char *out)
{
    unsigned i, n = 0;
    for (i = 0; i < 8 && fcb[i] != ' '; ++i) out[n++] = fcb[i];
    if (fcb[8] != ' ') { out[n++] = '.'; for (i = 8; i < 11 && fcb[i] != ' '; ++i) out[n++] = fcb[i]; }
    out[n] = 0;
}

static void utf8(const ofs_u16 *s, unsigned n)
{
    unsigned i;
    for (i = 0; i < n; ++i) {
        if (s[i] < 0x80) putchar(s[i]);
        else if (s[i] < 0x800) { putchar(0xC0 | s[i] >> 6); putchar(0x80 | (s[i] & 63)); }
        else { putchar(0xE0 | s[i] >> 12); putchar(0x80 | (s[i] >> 6 & 63)); putchar(0x80 | (s[i] & 63)); }
    }
}

static void list(struct ofs_ntfs_file *dir, const char *path)
{
    static struct ofs_ntfs_ent ent;
    struct ofs_ntfs_file sub;
    ofs_u32 pos = 0, next;
    char fcb[11], name[13], subpath[260];
    int r;
    printf("%s\n", path);
    while (!(r = ofs_ntfs_dir_next(&vol, dir, &pos, &ent))) {
        next = pos;
        assert(!ofs_ntfs_short(&vol, dir, &ent, fcb));
        name83(fcb, name);
        printf("  %-12s %02X %10lu%s %02u.%02u.%04u %02u:%02u  ", name, (unsigned)ent.attr,
               (unsigned long)ent.file.size, ent.file.big ? "+" : " ",
               ent.date & 31, ent.date >> 5 & 15, 1980 + (ent.date >> 9), ent.time >> 11, ent.time >> 5 & 63);
        utf8(ent.name, ent.name_len);
        putchar('\n');
        if (ent.attr & OFS_NTFS_DIR) {
            sub = ent.file;
            sprintf(subpath, "%s%s\\", path, name);
            list(&sub, subpath);
            pos = next;
        }
    }
    assert(r == OFS_NTFS_END);
}

static unsigned long crc_tab[256];

static unsigned long crc32(unsigned long c, const unsigned char *p, unsigned n)
{
    unsigned i, k;
    if (!crc_tab[1])
        for (i = 0; i < 256; ++i) {
            unsigned long v = i;
            for (k = 0; k < 8; ++k) v = v & 1 ? 0xEDB88320UL ^ v >> 1 : v >> 1;
            crc_tab[i] = v;
        }
    c ^= 0xFFFFFFFFUL;
    while (n--) c = crc_tab[(c ^ *p++) & 0xFF] ^ c >> 8;
    return c ^ 0xFFFFFFFFUL;
}

static void utf8s(const ofs_u16 *s, unsigned n, char *out)
{
    unsigned i;
    for (i = 0; i < n; ++i) {
        if (s[i] < 0x80) *out++ = (char)s[i];
        else if (s[i] < 0x800) { *out++ = (char)(0xC0 | s[i] >> 6); *out++ = (char)(0x80 | (s[i] & 63)); }
        else { *out++ = (char)(0xE0 | s[i] >> 12); *out++ = (char)(0x80 | (s[i] >> 6 & 63)); *out++ = (char)(0x80 | (s[i] & 63)); }
    }
    *out = 0;
}

static int failed;

static unsigned depth;

static void dump(struct ofs_ntfs_file *dir, const char *path)
{
    static struct ofs_ntfs_ent ent, found;
    static unsigned char buf[4096];
    struct ofs_ntfs_file sub;
    ofs_u32 pos = 0, next;
    char fcb[11], (*seen)[11] = NULL, sub_path[4096], name[800];
    ofs_u32 *seen_no = NULL;
    unsigned nseen = 0, cap = 0, i;
    int r;
    while (!(r = ofs_ntfs_dir_next(&vol, dir, &pos, &ent))) {
        next = pos;
        utf8s(ent.name, ent.name_len, name);
        sprintf(sub_path, "%s/%s", path, name);
        if (ofs_ntfs_short(&vol, dir, &ent, fcb)) { printf("! %s: no 8.3 name\n", sub_path); failed = 1; }
        for (i = 0; i < nseen; ++i)
            if (!memcmp(seen[i], fcb, 11) && seen_no[i] != ent.file.first) { printf("! %s: 8.3 name %.11s twice\n", sub_path, fcb); failed = 1; }
        if (nseen == cap) {
            cap = cap ? cap * 2 : 64;
            seen = realloc(seen, cap * 11); seen_no = realloc(seen_no, cap * sizeof(*seen_no));
            assert(seen && seen_no);
        }
        seen_no[nseen] = ent.file.first;
        memcpy(seen[nseen++], fcb, 11);
        if (!getenv("NTFS_NO_LOOKUP") && (ofs_ntfs_lookup(&vol, dir, fcb, &found) || found.file.first != ent.file.first)) {
            printf("! %s: lookup of %.11s fails\n", sub_path, fcb); failed = 1;
        }
        if (ent.attr & OFS_NTFS_DIR) {
            printf("D %s\n", sub_path);
            sub = ent.file;
            if (depth == 64) { printf("! %s: deeper than 64 levels\n", sub_path); failed = 1; break; }
            ++depth;
            dump(&sub, sub_path);
            --depth;
        } else {
            unsigned long c = 0;
            ofs_fsize off = 0;
            ofs_u16 done;
            do {
                r = ofs_ntfs_read(&vol, &ent.file, off, buf, sizeof(buf), &done);
                if (r) break;
                c = crc32(c, buf, done);
                off += done;
            } while (done && off < 256UL << 20);
            /* larger than any test file: a damaged size (fuzzing), the
               rest would be zeros */
            if (!r && done) { printf("! %s: larger than 256 MB\n", sub_path); failed = 1; }
            else if (r == OFS_NTFS_ECOMP) printf("C %s\n", sub_path);
            else if (r) { printf("! %s: read error %d at %lu\n", sub_path, r, (unsigned long)off); failed = 1; }
            else printf("F %s %lu %08lx\n", sub_path, (unsigned long)off, c);
        }
        pos = next;
    }
    if (r != OFS_NTFS_END) { printf("! %s: directory error %d\n", path, r); failed = 1; }
    free(seen); free(seen_no);
}

static void cat(const char *path)
{
    static struct ofs_ntfs_ent ent;
    struct ofs_ntfs_file dir;
    char fcb[11], part[13];
    static char buf[4096];
    const char *p = path, *e;
    ofs_u32 off = 0;
    ofs_u16 done;
    unsigned i, n;
    int r;
    ofs_ntfs_rootdir(&vol, &dir);
    for (;;) {
        e = strchr(p, '\\');
        n = e ? (unsigned)(e - p) : (unsigned)strlen(p);
        memcpy(part, p, n); part[n] = 0;
        memset(fcb, ' ', 11);
        e = strchr(part, '.');
        for (i = 0; part[i] && part + i != e && i < 8; ++i) fcb[i] = part[i];
        if (e) for (i = 0; e[1+i] && i < 3; ++i) fcb[8+i] = e[1+i];
        if (ofs_ntfs_lookup(&vol, &dir, fcb, &ent)) { fprintf(stderr, "not found: %s\n", part); exit(1); }
        p += n;
        if (!*p) break;
        ++p; dir = ent.file;
    }
    do {
        r = ofs_ntfs_read(&vol, &ent.file, off, buf, sizeof(buf), &done);
        if (r) { fprintf(stderr, "read: %d\n", r); exit(1); }
        fwrite(buf, 1, done, stdout);
        off += done;
    } while (done);
}

int main(int argc, char **argv)
{
            ofs_u32 start = argc > 2 ? strtoul(argv[2], NULL, 0) : 0, free_clusters;
    struct ofs_ntfs_file root;
    int r;
    if (argc < 2) { fprintf(stderr, "usage: ntfs_test IMAGE [LBA [PATH]]\n"); return 2; }
    image = fopen(argv[1], "rb");
    assert(image);
    if (getenv("NTFS_SECTOR")) {           /* the image's sector size */
        unsigned long ss = strtoul(getenv("NTFS_SECTOR"), NULL, 0);
        for (sshift = 9; (1UL << sshift) < ss; ++sshift) ;
    }
    r = ofs_ntfs_mount(&vol, img_read, NULL, start, sshift, test_cp(850), sector_buf);
    if (r) { fprintf(stderr, "mount: %d\n", r); return 1; }
    if (argc > 3 && !strcmp(argv[3], "--dump")) {
        ofs_ntfs_rootdir(&vol, &root);
        dump(&root, "");
        return failed;
    }
    if (argc > 3) { cat(argv[3]); return 0; }
    assert(!ofs_ntfs_free(&vol, &free_clusters));
    printf("label \""); utf8(vol.label, vol.label_len);
    printf("\" serial %04lX-%04lX, %lu of %lu clusters of %u bytes free%s\n",
           (unsigned long)(vol.serial >> 16), (unsigned long)(vol.serial & 0xFFFF),
           (unsigned long)free_clusters, (unsigned long)vol.clusters, 512u << vol.cshift, vol.dirty ? ", dirty" : "");
    ofs_ntfs_rootdir(&vol, &root);
    list(&root, "\\");
    fprintf(stderr, "%lu sector reads\n", reads);
    return 0;
}
