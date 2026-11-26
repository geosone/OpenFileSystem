/* ofs_bmap through the common interface (src/vol.c): a file of an image
   (FAT, exFAT or NTFS, made by the Linux tools) mapped sector by sector;
   each sector read straight from the image where ofs_bmap says must be
   what ofs_read reads there, past the end and unaligned are OFS_ERANGE.
     bmap_test IMAGE SECTOR_SIZE NAME [resident]
   NAME a file in the root directory (ASCII); with "resident" the file
   must have no sectors of its own (OFS_ENOTSUP, NTFS's resident data). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "testcp.h"
#include "ofs/vol.h"

static FILE *img;
static unsigned ssize;

static int rd(void *dev, ofs_lba lba, void *buf)
{
    (void)dev;
    if (fseek(img, (long)(lba * ssize), SEEK_SET) || fread(buf, ssize, 1, img) != 1) return -1;
    return 0;
}

static struct ofs_vol vol;
static ofs_u8 vbuf[OFS_VOL_BUF];
static ofs_u8 raw[4096], via[4096];

int main(int argc, char **argv)
{
    struct ofs_host h;
    struct ofs_node root;
    struct ofs_dirent de;
    ofs_u16 name[64];
    unsigned len, sh = 0;
    ofs_fsize off;
    ofs_lba lba;
    ofs_u32 done;
    unsigned long n = 0;
    int r;
    if (argc < 4) { fputs("bmap_test IMAGE SECTOR_SIZE NAME [resident]\n", stderr); return 2; }
    ssize = (unsigned)atoi(argv[2]);
    while ((1u << sh) < ssize) ++sh;
    if (!(img = fopen(argv[1], "rb"))) { perror(argv[1]); return 1; }
    memset(&h, 0, sizeof(h));
    h.read = rd;
    h.sshift = (ofs_u8)sh;
    if ((r = ofs_mount(&vol, &h, NULL, 0, test_cp(850), vbuf))) { fprintf(stderr, "mount: %d\n", r); return 1; }
    ofs_root(&vol, &root);
    for (len = 0; argv[3][len] && len < 64; ++len) name[len] = (ofs_u16)(unsigned char)argv[3][len];
    if ((r = ofs_lookup(&vol, &root, name, len, &de))) { fprintf(stderr, "%s: %d\n", argv[3], r); return 1; }
    if (argc > 4) {
        r = ofs_bmap(&vol, &de.node, 0, &lba);
        if (r != OFS_ENOTSUP) { fprintf(stderr, "%s %s: resident, but bmap %d\n", vol.type, argv[3], r); return 1; }
        printf("bmap %s %s: resident, no sectors\n", vol.type, argv[3]);
        return 0;
    }
    for (off = 0; off < de.node.size; off += ssize, ++n) {
        if ((r = ofs_bmap(&vol, &de.node, off, &lba))) { fprintf(stderr, "bmap at %lu: %d\n", (unsigned long)off, r); return 1; }
        if (rd(NULL, lba, raw)) { fprintf(stderr, "sector %lu: no\n", (unsigned long)lba); return 1; }
        memset(via, 0, sizeof(via));
        if ((r = ofs_read(&vol, &de.node, off, via, ssize, &done))) { fprintf(stderr, "read: %d\n", r); return 1; }
        if (memcmp(raw, via, done)) {
            fprintf(stderr, "%s %s: byte %lu -> sector %lu holds other data\n", vol.type, argv[3], (unsigned long)off,
                    (unsigned long)lba);
            return 1;
        }
    }
    if (ofs_bmap(&vol, &de.node, off, &lba) != OFS_ERANGE || (de.node.size > 1 && ofs_bmap(&vol, &de.node, 1, &lba) != OFS_ERANGE)) {
        fprintf(stderr, "%s %s: past the end or unaligned not OFS_ERANGE\n", vol.type, argv[3]);
        return 1;
    }
    printf("bmap %s %s: %lu sectors where ofs_read finds them\n", vol.type, argv[3], n);
    return 0;
}
