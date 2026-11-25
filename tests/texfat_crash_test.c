/* TexFAT transactions against power failures: an exFAT volume with two
   FATs in memory (formatted by exfat_format.c) gets a base state, then
   two transactions through the common interface (src/vol.c): new files
   and directories, appending, a directory growing past its first
   clusters, renames across directories, removals, truncation, each
   committed by ofs_sync. They are run once in full (the states S0, S1,
   S2 and the number of sector writes), then again for every n from 0 to
   that number with the power failing after the n-th write (the later
   writes are lost). After each failure the volume is mounted again: its
   tree must be S0, S1 or S2 exactly, never a mix, a further transaction
   must work, and the check (exfat_check.c) must find no error.
     texfat_crash_test [STEP]      (every STEP-th n; default 1) */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "testcp.h"
#include "ofs/vol.h"
#include "ofs/exfat_check.h"
#include "ofs/exfat_format.h"

#define SECTORS 16384UL                 /* 8 MB */
static unsigned char disk[SECTORS * 512], base[SECTORS * 512];
static unsigned long writes, budget;    /* budget: writes until the power fails */
static int failing;

static int rd(void *dev, ofs_lba lba, void *buf)
{
    (void)dev;
    if (lba >= SECTORS) return -1;
    memcpy(buf, disk + lba * 512, 512);
    return 0;
}

static int wr(void *dev, ofs_lba lba, const void *buf)
{
    (void)dev;
    if (lba >= SECTORS) return -1;
    if (failing && writes >= budget) return 0;  /* lost: the power is gone */
    ++writes;
    memcpy(disk + lba * 512, buf, 512);
    return 0;
}

static int fmt_wr(void *dev, ofs_lba lba, unsigned count, const void *buf)
{
    (void)dev;
    memcpy(disk + (unsigned long)lba * 512, buf, count * 512UL);
    return 0;
}

static struct ofs_vol vol;
static ofs_u8 vbuf[OFS_VOL_BUF];

static void mount(void)
{
    struct ofs_host h;
    memset(&h, 0, sizeof(h));
    h.read = rd;
    h.write = wr;
    assert(!ofs_mount(&vol, &h, NULL, 0, test_cp(850), vbuf));
    assert(!strcmp(vol.type, "exFAT") && vol.u.ex.tex);
}

/* ---- paths ---- */

static unsigned to16(const char *s, ofs_u16 *o)
{
    unsigned n = 0;
    while (*s) o[n++] = (unsigned char)*s++;
    return n;
}

/* the directory of path's last part (in *dir), that part in name/len */
static int parent(const char *path, struct ofs_node *dir, ofs_u16 *name, unsigned *len)
{
    static struct ofs_dirent de;
    char part[256];
    const char *p = path, *q;
    int r;
    ofs_root(&vol, dir);
    for (;;) {
        for (q = p; *q && *q != '/'; ++q)
            ;
        memcpy(part, p, (size_t)(q - p));
        part[q - p] = 0;
        if (!*q) { *len = to16(part, name); return 0; }
        {
            ofs_u16 n16[256];
            unsigned k = to16(part, n16);
            if ((r = ofs_lookup(&vol, dir, n16, k, &de))) return r;
        }
        *dir = de.node;
        p = q + 1;
    }
}

static int node(const char *path, struct ofs_node *n)
{
    static struct ofs_dirent de;
    struct ofs_node dir;
    ofs_u16 name[256];
    unsigned len;
    int r;
    if ((r = parent(path, &dir, name, &len))) return r;
    if ((r = ofs_lookup(&vol, &dir, name, len, &de))) return r;
    *n = de.node;
    return 0;
}

/* ---- operations: the first error ends a run (a lost write may show) ---- */

static int failed;
#define OP(x) do { if (!failed && (x)) failed = 1; } while (0)

static void mk(const char *path, int is_dir)
{
    static struct ofs_dirent de;
    struct ofs_node dir;
    ofs_u16 name[256];
    unsigned len;
    if (failed) return;
    OP(parent(path, &dir, name, &len));
    OP(ofs_create(&vol, &dir, name, len, is_dir, &de));
}

static void put(const char *path, ofs_fsize off, ofs_u32 n, unsigned seed)
{
    static ofs_u8 buf[70000];
    struct ofs_node f;
    ofs_u32 i;
    if (failed) return;
    for (i = 0; i < n; ++i) buf[i] = (ofs_u8)(seed * 31 + (off + i) * 7 + ((off + i) >> 9));
    OP(node(path, &f));
    OP(ofs_write(&vol, &f, off, buf, n));
}

static void rm(const char *path)
{
    struct ofs_node dir;
    ofs_u16 name[256];
    unsigned len;
    if (failed) return;
    OP(parent(path, &dir, name, &len));
    OP(ofs_remove(&vol, &dir, name, len));
}

static void mv(const char *from, const char *to)
{
    struct ofs_node d1, d2;
    ofs_u16 n1[256], n2[256];
    unsigned l1, l2;
    if (failed) return;
    OP(parent(from, &d1, n1, &l1));
    OP(parent(to, &d2, n2, &l2));
    OP(ofs_rename(&vol, &d1, n1, l1, &d2, n2, l2));
}

static void cut(const char *path, ofs_fsize size)
{
    struct ofs_node f;
    if (failed) return;
    OP(node(path, &f));
    OP(ofs_resize(&vol, &f, size));
}

static void commit(void) { OP(ofs_sync(&vol)); }

static void t0(void)
{
    char p[64];
    unsigned i;
    mk("D", 1);
    mk("a.txt", 0); put("a.txt", 0, 10000, 1);
    mk("D/b.txt", 0); put("D/b.txt", 0, 5000, 2);
    mk("weg.bin", 0); put("weg.bin", 0, 60000, 3);
    for (i = 0; i < 40; ++i) { sprintf(p, "D/Datei mit langem Namen %02u.txt", i); mk(p, 0); put(p, 0, 100 + i, 4 + i); }
    commit();
}

static void t1(void)
{
    char p[64];
    unsigned i;
    mk("D/neu.txt", 0); put("D/neu.txt", 0, 20000, 50);
    put("a.txt", 10000, 3000, 51);              /* appended */
    mv("D/b.txt", "c.txt");                     /* to another directory */
    mk("E", 1);
    for (i = 0; i < 60; ++i) { sprintf(p, "E/Eintrag mit sehr langem Namen Nummer %02u", i); mk(p, 0); put(p, 0, 10 + i, 60 + i); }
    for (i = 0; i < 40; i += 4) { sprintf(p, "D/Datei mit langem Namen %02u.txt", i); rm(p); }
    rm("weg.bin");
    commit();
}

static void t2(void)
{
    char p[64];
    unsigned i;
    cut("D/neu.txt", 3000);
    mk("F", 1);
    mv("E/Eintrag mit sehr langem Namen Nummer 07", "F/sieben");
    for (i = 10; i < 60; i += 2) { sprintf(p, "E/Eintrag mit sehr langem Namen Nummer %02u", i); rm(p); }
    mk("F/gross.bin", 0); put("F/gross.bin", 0, 65000, 99);
    mv("D", "F/D");                             /* a directory moved */
    commit();
}

/* ---- the state: names, sizes and contents, independent of order ---- */

static unsigned long hash(unsigned long h, const void *p, size_t n)
{
    const unsigned char *c = p;
    while (n--) h = (h ^ *c++) * 16777619UL;
    return h;
}

static unsigned long tree(struct ofs_node *dir, unsigned long prefix)
{
    struct ofs_dirent de;
    static ofs_u8 buf[70000];
    unsigned long sum = 0;
    ofs_u32 pos = 0, done;
    int r;
    while (!(r = ofs_next(&vol, dir, &pos, &de))) {
        unsigned long h = hash(prefix, de.name, de.name_len * 2u);
        h = hash(h, &de.node.dir, 1);
        if (de.node.dir) h += tree(&de.node, h);
        else {
            ofs_fsize off = 0;
            unsigned long size = (unsigned long)de.node.size;
            h = hash(h, &size, sizeof(size));
            while (off < de.node.size) {
                if (ofs_read(&vol, &de.node, off, buf, sizeof(buf), &done) || !done) return 0xDEAD;
                h = hash(h, buf, done);
                off += done;
            }
        }
        sum += h * 2654435761UL;
    }
    return r == OFS_END ? sum : 0xBAD;
}

static unsigned long state(void)
{
    struct ofs_node root;
    ofs_root(&vol, &root);
    return tree(&root, 2166136261UL);
}

/* ---- the check ---- */

static unsigned char bits[SECTORS / 8 + 1];
static int mark(void *ctx, ofs_u32 bit)
{
    int old = bits[bit >> 3] >> (bit & 7) & 1;
    (void)ctx;
    bits[bit >> 3] |= (unsigned char)(1 << (bit & 7));
    return old;
}
static void clear(void *ctx) { (void)ctx; memset(bits, 0, sizeof(bits)); }
static void msg(void *ctx, int level, const char *text)
{
    (void)ctx;
    if (level == OFS_EXCHK_ERROR) fprintf(stderr, "  check: %s\n", text);
}

static int check(void)
{
    struct ofs_exchk c;
    memset(&c, 0, sizeof(c));
    c.read = rd; c.cp = test_cp(850); c.msg = msg; c.mark = mark; c.clear = clear; c.window = SECTORS;
    if (ofs_exchk_run(&c)) return -1;
    return (int)c.errors;
}

int main(int argc, char **argv)
{
    struct ofs_exfmt f;
    static ofs_u8 fbuf[16 * 512];
    unsigned long s0, s1, s2, total, n, step = argc > 1 ? strtoul(argv[1], NULL, 0) : 1, seen[3] = { 0, 0, 0 };

    memset(&f, 0, sizeof(f));
    f.sectors = SECTORS; f.cluster = 4096; f.nfats = 2; f.serial = 0x12345678UL;
    f.label_len = (ofs_u8)to16("TEXFAT", f.label);
    assert(!ofs_exfmt_layout(&f));
    assert(!ofs_exfmt_write(&f, fmt_wr, NULL, NULL, 0, fbuf, 16));
    mount();
    t0();
    assert(!failed);
    s0 = state();
    memcpy(base, disk, sizeof(disk));
    /* the run without a failure, from a fresh mount as the others (its
       first change copies the FAT and bitmap to the working ones) */
    mount();
    writes = 0;
    t1();
    assert(!failed);
    s1 = state();
    t2();
    assert(!failed);
    s2 = state();
    total = writes;
    assert(s0 != s1 && s1 != s2 && s0 != s2);
    /* the full run is a valid volume */
    mount();
    assert(state() == s2);
    if (check()) { fprintf(stderr, "the full run: check errors\n"); return 1; }
    printf("TexFAT: %lu sector writes in two transactions\n", total);

    for (n = 0; n <= total; n += step) {
        unsigned long s;
        int k;
        memcpy(disk, base, sizeof(disk));
        failing = 1; budget = n; writes = 0; failed = 0;
        mount();
        t1();
        t2();
        failing = 0; failed = 0;
        /* the power is back */
        mount();
        s = state();
        k = s == s0 ? 0 : s == s1 ? 1 : s == s2 ? 2 : -1;
        if (k < 0) { fprintf(stderr, "power failure after write %lu: a state of neither transaction\n", n); return 1; }
        ++seen[k];
        /* a transaction after it, then the check */
        mk("nachher.txt", 0); put("nachher.txt", 0, 1234, 7); rm("nachher.txt"); commit();
        if (failed) { fprintf(stderr, "power failure after write %lu: no transaction after it\n", n); return 1; }
        if (state() != s) { fprintf(stderr, "power failure after write %lu: the state changed\n", n); return 1; }
        if (check()) { fprintf(stderr, "power failure after write %lu: check errors (state S%d)\n", n, k); return 1; }
    }
    printf("TexFAT power failures passed: %lu cases, S0 %lu, S1 %lu, S2 %lu\n",
           seen[0] + seen[1] + seen[2], seen[0], seen[1], seen[2]);
    return 0;
}
