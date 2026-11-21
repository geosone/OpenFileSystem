/* exFAT beyond 32 bits on the host (64-bit sector numbers and sizes):
   exfat_big_test IMAGE      on a volume of more than 2^32 sectors: a file
                             in clusters past 2 TiB and a file of more than
                             4 GiB, written and read back
   exfat_big_test -v FILE N  FILE (read through the Linux driver) holds
                             pattern N of its size */
#define _XOPEN_SOURCE 500                /* fseeko */
#define _FILE_OFFSET_BITS 64
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "testcp.h"
#include "ofs/exfat.h"

#define GIB4 ((ofs_u64)1 << 32)

static FILE *image;
static struct ofs_exfat vol;
static ofs_u8 sector_buf[OFS_EXFAT_MAXSECT];
static struct ofs_exfat_ent ent;
static ofs_u64 top;                    /* the highest sector written */

static int seek(FILE *f, ofs_u64 off)
{
    return fseeko(f, (off_t)off, SEEK_SET);
}

static int img_read(void *dev, ofs_lba lba, void *buf)
{
    (void)dev;
    if (seek(image, (ofs_u64)lba << vol.sshift)) return -1;
    return fread(buf, 1u << (vol.sshift ? vol.sshift : 9), 1, image) == 1 ? 0 : -1;
}

static int img_write(void *dev, ofs_lba lba, const void *buf)
{
    (void)dev;
    if (lba > top) top = lba;
    if (seek(image, (ofs_u64)lba << vol.sshift)) return -1;
    return fwrite(buf, 1u << vol.sshift, 1, image) == 1 ? 0 : -1;
}

#define CHECK(x) do { int r_ = (x); if (r_) { fprintf(stderr, "%s:%d: %s = %d\n", __FILE__, __LINE__, #x, r_); exit(1); } } while (0)

static ofs_u8 pattern(unsigned n, ofs_u64 off)
{
    return (ofs_u8)(n * 31 + off * 7 + (off >> 12) + (off >> 32) * 13);
}

static unsigned name16(const char *s, ofs_u16 *out)
{
    unsigned k = 0;
    while (*s) out[k++] = (ofs_u8)*s++;
    return k;
}

static void find(const char *name)
{
    struct ofs_exfat_file root;
    ofs_u16 n[64];
    unsigned len = name16(name, n);
    ofs_u32 pos = 0;
    ofs_exfat_rootdir(&vol, &root);
    while (!ofs_exfat_dir_next(&vol, &root, &pos, &ent))
        if (ent.name_len == len && !memcmp(ent.name, n, len * 2)) return;
    fprintf(stderr, "not found: %s\n", name);
    exit(1);
}

/* a new file of size bytes of pattern n */
static void make(const char *name, ofs_u64 size, unsigned n)
{
    static ofs_u8 buf[65536];
    struct ofs_exfat_file root, f;
    struct ofs_exfat_where rw, w;
    ofs_u16 nm[64], done;
    ofs_u32 pos, i, k;
    ofs_u64 off;
    ofs_exfat_rootdir(&vol, &root);
    memset(&rw, 0, sizeof(rw));
    rw.none = 1;
    CHECK(ofs_exfat_create(&vol, &root, &rw, nm, name16(name, nm), OFS_EXFAT_ARCH, 0x5000, 0x5A21, &pos));
    find(name);
    f = ent.file;
    ofs_exfat_where_of(&root, ent.pos, &w);
    for (off = 0; off < size; off += k) {
        k = size - off < 65536 ? (ofs_u32)(size - off) : 65536;
        for (i = 0; i < k; ++i) buf[i] = pattern(n, off + i);
        CHECK(ofs_exfat_write(&vol, &f, &w, off, buf, (ofs_u16)(k == 65536 ? 65535 : k), &done));
        k = done;
    }
}

/* the file read back: whole (step 1) or every step-th 64 KiB */
static void check(const char *name, ofs_u64 size, unsigned n, unsigned step)
{
    static ofs_u8 buf[65535];
    ofs_u64 off;
    ofs_u16 done, i;
    unsigned c = 0;
    find(name);
    assert(ent.file.size == size && ent.file.valid == size);
    for (off = 0; off < size; off += 65535, ++c) {
        if (c % step && off + 65535 < size) continue;
        CHECK(ofs_exfat_read(&vol, &ent.file, off, buf, sizeof(buf), &done));
        assert(done == (size - off < 65535 ? size - off : 65535));
        for (i = 0; i < done; ++i)
            if (buf[i] != pattern(n, off + i)) { fprintf(stderr, "%s differs at %llu\n", name, (unsigned long long)(off + i)); exit(1); }
    }
}

static int verify(const char *path, unsigned n)
{
    static ofs_u8 buf[1 << 20];
    FILE *f = fopen(path, "rb");
    ofs_u64 off = 0;
    size_t got, i;
    if (!f) { perror(path); return 1; }
    while ((got = fread(buf, 1, sizeof(buf), f)) > 0) {
        for (i = 0; i < got; ++i)
            if (buf[i] != pattern(n, off + i)) { fprintf(stderr, "%s differs at %llu\n", path, (unsigned long long)(off + i)); return 1; }
        off += got;
    }
    fclose(f);
    printf("%s: %llu bytes of pattern %u\n", path, (unsigned long long)off, n);
    return 0;
}

int main(int argc, char **argv)
{
    const ofs_u64 big = GIB4 + 1000003;     /* 4 GiB and a bit */
    if (argc == 4 && !strcmp(argv[1], "-v")) return verify(argv[2], (unsigned)atoi(argv[3]));
    if (argc != 2) { fprintf(stderr, "exfat_big_test IMAGE | -v FILE N\n"); return 2; }
    image = fopen(argv[1], "r+b");
    assert(image);
    CHECK(ofs_exfat_mount(&vol, img_read, NULL, 0, test_cp(850), sector_buf));
    vol.write = img_write;
    /* more than 2^32 sectors with 512-byte sectors; with 4 KiB sectors
       that takes more than 16 TiB (past what ext4 allows for a file), so
       there it is the same volume: past 2 TiB, 64-bit byte offsets */
    assert(vol.sshift != 9 || vol.heap + ((ofs_u64)vol.clusters << vol.cshift) > GIB4);
    /* past 2^32 sectors: the clusters are taken from the hint on */
    vol.hint = vol.clusters - 16;
    top = 0;
    make("HIGH.BIN", 3000000, 1);
    assert(((ofs_u64)top << vol.sshift) > ((ofs_u64)2 << 40));   /* past 2 TiB */
    assert(vol.sshift != 9 || top >= GIB4);
    /* larger than 4 GiB: 64-bit size and valid data length */
    vol.hint = 2;
    make("BIG.BIN", big, 2);
    CHECK(ofs_exfat_clean(&vol));
    /* again from the medium */
    CHECK(ofs_exfat_mount(&vol, img_read, NULL, 0, test_cp(850), sector_buf));
    check("HIGH.BIN", 3000000, 1, 1);
    check("BIG.BIN", big, 2, 97);
    printf("exFAT big test passed: last sector written %llu\n", (unsigned long long)top);
    fclose(image);
    return 0;
}
