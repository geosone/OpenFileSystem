/* exFAT DEFRAG on the host:
   exdefrag_test IMAGE [MODE [SECTORS]]
   MODE: A analyze (default), U only the fragmented files, F full, T the
   tree (path, size and CRC-32 of every file, read through exfat.c: also
   for TexFAT, which the Linux driver does not read).
   SECTORS: power loss after that many sectors written (exit 3).
   Prints the counts and "gaps": free runs below the last cluster in use. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ofs/exfat_defrag.h"

#define MEM (8UL << 20)

static FILE *image;
static unsigned char *mem;
static unsigned long budget = (unsigned long)-1, written;

static int img_read(void *dev, ofs_lba lba, void *buf)
{
    (void)dev;
    if (fseek(image, (long)lba * 512, SEEK_SET)) return -1;
    return fread(buf, 512, 1, image) == 1 ? 0 : -1;
}

static int img_write(void *dev, ofs_lba lba, const void *buf)
{
    (void)dev;
    if (fseek(image, (long)lba * 512, SEEK_SET)) return -1;
    if (written >= budget) {                    /* the power has failed */
        fclose(image);
        printf("power loss after %lu sectors\n", budget);
        exit(3);
    }
    ++written;
    return fwrite(buf, 512, 1, image) == 1 ? 0 : -1;
}

static void *at(void *ctx, ofs_u32 off) { (void)ctx; return mem + off; }

/* ---- T: the tree ---- */

static struct ofs_exfat V;
static unsigned long crc_tab[256];

static unsigned long crc32(unsigned long c, const unsigned char *p, unsigned n)
{
    while (n--) c = crc_tab[(c ^ *p++) & 0xFF] ^ (c >> 8);
    return c;
}

static int tree(struct ofs_exfat_file *dir, const char *path, unsigned depth)
{
    static unsigned char buf[16384];
    struct ofs_exfat_ent e;
    struct ofs_exfat_file sub;
    ofs_u32 pos = 0;
    ofs_fsize off;
    ofs_u16 done;
    unsigned long c;
    char name[1024];
    unsigned i, k;
    int r;
    if (depth > 64) return -1;
    for (;;) {
        r = ofs_exfat_dir_next(&V, dir, &pos, &e);
        if (r == OFS_EXFAT_END) return 0;
        if (r) return r;
        k = (unsigned)strlen(path);
        memcpy(name, path, k);
        name[k++] = '/';
        for (i = 0; i < e.name_len && k < sizeof(name) - 4; ++i)   /* UTF-8, enough for a compare */
            if (e.name[i] < 0x80) name[k++] = (char)e.name[i];
            else if (e.name[i] < 0x800) { name[k++] = (char)(0xC0 | e.name[i] >> 6); name[k++] = (char)(0x80 | (e.name[i] & 0x3F)); }
            else { name[k++] = (char)(0xE0 | e.name[i] >> 12); name[k++] = (char)(0x80 | (e.name[i] >> 6 & 0x3F)); name[k++] = (char)(0x80 | (e.name[i] & 0x3F)); }
        name[k] = 0;
        if (e.attr & OFS_EXFAT_DIR) {
            printf("%s/\n", name);
            sub = e.file;
            sub.root = (ofs_u8)!sub.nofat;
            if ((r = tree(&sub, name, depth + 1)) != 0) return r;
            continue;
        }
        c = 0xFFFFFFFFUL;
        for (off = 0; off < e.file.size; off += done) {
            ofs_u16 want = (ofs_u16)(e.file.size - off > sizeof(buf) ? sizeof(buf) : e.file.size - off);
            if ((r = ofs_exfat_read(&V, &e.file, off, buf, want, &done)) != 0 || !done) return r ? r : -1;
            c = crc32(c, buf, done);
        }
        printf("%s %lu %08lx\n", name, (unsigned long)e.file.size, c ^ 0xFFFFFFFFUL);
    }
}

static int show_tree(void)
{
    static ofs_u8 sec[OFS_EXFAT_MAXSECT];
    struct ofs_exfat_file root;
    unsigned long c;
    unsigned i, k;
    for (i = 0; i < 256; ++i) {
        for (c = i, k = 0; k < 8; ++k) c = c & 1 ? 0xEDB88320UL ^ (c >> 1) : c >> 1;
        crc_tab[i] = c;
    }
    if (ofs_exfat_mount(&V, img_read, NULL, 0, NULL, sec)) { puts("not exFAT"); return 2; }
    ofs_exfat_rootdir(&V, &root);
    if (tree(&root, "", 0)) { puts("tree unreadable"); return 2; }
    return 0;
}

int main(int argc, char **argv)
{
    static ofs_defrag_idx order[65535];
    static ofs_u8 sec[OFS_EXFAT_MAXSECT];
    struct ofs_exdefrag d;
    ofs_u32 c, last = 0, gaps = 0;
    int r, mode = argc > 2 ? argv[2][0] : 'A';
    if (argc < 2) { fputs("exdefrag_test IMAGE [A|U|F|T [SECTORS]]\n", stderr); return 2; }
    if (argc > 3) budget = strtoul(argv[3], NULL, 0);
    image = fopen(argv[1], mode == 'A' || mode == 'T' ? "rb" : "r+b");
    if (!image) { perror(argv[1]); return 2; }
    if (mode == 'T') return show_tree();
    mem = malloc(MEM);
    memset(&d, 0, sizeof(d));
    d.read = img_read; d.write = img_write;
    d.mode = mode == 'F' ? OFS_DEFRAG_FULL : mode == 'U' ? OFS_DEFRAG_FILES : OFS_DEFRAG_ANALYZE;
    d.mem = at; d.memsize = MEM; d.block = 1UL << 30;
    d.order = order; d.maxitems = 65535;
    d.buf = sec;
    r = ofs_exdefrag_run(&d);
    if (r) { printf("stopped: %d\n", r); return 2; }
    for (c = 2; c < d.clusters + 2; ++c) if (ofs_exdefrag_used(c)) last = c;
    for (c = 2; c < last; ++c) if (!ofs_exdefrag_used(c) && ofs_exdefrag_used(c + 1)) ++gaps;
    printf("%s: %lu files, %lu directories, %lu clusters, %lu used\n", d.tex ? "TexFAT" : "exFAT",
           (unsigned long)d.files, (unsigned long)d.dirs, (unsigned long)d.clusters, (unsigned long)d.used);
    printf("fragmented %lu, after %lu, stuck %lu; moves %lu, clusters moved %lu, sectors written %lu; gaps %lu\n",
           (unsigned long)d.fragmented, (unsigned long)d.fragmented_after, (unsigned long)d.stuck,
           (unsigned long)d.moves, (unsigned long)d.moved, written, (unsigned long)gaps);
    fclose(image);
    return 0;
}
