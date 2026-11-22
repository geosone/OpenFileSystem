/* exFAT and TexFAT defragmentation; the order of the writes is described
   in include/ofs/exfat_defrag.h.  As fat_defrag.c: the files and
   directories are read into a list (32 bytes each: first cluster,
   clusters, the parent directory and the place of the entry set in it),
   the clusters in use into a bit map.  FILES: every fragmented file goes
   to the lowest free run that holds it.  FULL: the same first (to the
   highest), then the directories and after them the files, in the order
   of the directory tree, are placed one after the other from cluster 2
   on; a file in the way goes to the highest free run first.  Clusters of
   no movable file (the root directory, the bitmaps, the up-case table,
   files still fragmented or stuck) stay where they are.  Last, the
   directories still fragmented (TexFAT's copied clusters) are joined, the
   deepest first. */
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include "ofs/exfat_defrag.h"

/* under DOS without OFS_EXFAT_RELOCATE: nothing (an ISO C unit is not empty) */
typedef int ofs_exdefrag_unit;

#ifdef OFS_EXFAT_HAS_RELOCATE

#ifdef __WATCOMC__
#define FMEMSET  _fmemset
#define FMEMMOVE _fmemmove
#else
#define FMEMSET  memset
#define FMEMMOVE memmove
#endif
#define FAR OFS_DEFRAG_FAR

#define NONE     0xFFFFFFFFUL           /* no item: the parent of the root's entries */
#define MAXDEPTH 64
#define ISIZE    32                     /* bytes per item */
#define F_DIR    1
#define F_FRAG   2
#define F_NOFAT  4
#define F_STUCK  8                      /* its entry set cannot be switched safely */

struct item { ofs_u32 first, count, parent, pos; ofs_u8 flags, pad[15]; };
typedef char item_is_32_bytes[sizeof(struct item) == ISIZE ? 1 : -1];

static struct ofs_exdefrag *D;
static struct ofs_exfat V;
static struct ofs_exfat_ent ent;
static unsigned nitems, cb;             /* cb: bytes per cluster, a shift */
static ofs_u32 nclu, bmsize;
static char text[120];

static void say(const char *fmt, ...)
{
    va_list a;
    if (!D->status) return;
    va_start(a, fmt);
    vsprintf(text, fmt, a);
    va_end(a);
    D->status(D->ctx, text);
}

static void show(ofs_u32 clu, ofs_u32 count, int state) { if (D->map && count) D->map(D->ctx, clu, count, state); }
static int stopped(void) { return D->stop && D->stop(D->ctx); }

/* exfat.c's errors as DEFRAG's */
static int err(int r)
{
    if (r == OFS_EXFAT_OK) return OFS_DEFRAG_OK;
    if (r == OFS_EXFAT_EIO || r == OFS_EXFAT_EWRITE) return OFS_DEFRAG_EIO;
    return OFS_DEFRAG_EDIR;
}

/* ---- work memory: the bit map, then the list ---- */

static ofs_u8 FAR *at(ofs_u32 off) { return (ofs_u8 FAR *)D->mem(D->ctx, off); }
static struct item FAR *item(ofs_u32 i) { return (struct item FAR *)at(bmsize + i * ISIZE); }

static int used(ofs_u32 c) { return *at(c >> 3) >> (unsigned)(c & 7) & 1; }

int ofs_exdefrag_used(ofs_u32 clu) { return clu >= 2 && clu < nclu + 2 && used(clu); }

static void set_used(ofs_u32 c, ofs_u32 n, int on)
{
    ofs_u8 FAR *p;
    for (; n; --n, ++c) {
        p = at(c >> 3);
        if (on) *p |= (ofs_u8)(1 << (unsigned)(c & 7));
        else *p &= (ofs_u8)~(1 << (unsigned)(c & 7));
    }
}

/* the first cluster from c on (below end) whose bit is not state */
static ofs_u32 skip(ofs_u32 c, ofs_u32 end, int state)
{
    ofs_u8 all = (ofs_u8)(state ? 0xFF : 0);
    ofs_u8 FAR *p;
    ofs_u32 left;
    while (c < end) {
        if (!(c & 7) && end - c >= 8) {
            p = at(c >> 3);
            left = D->block - ((c >> 3) & (D->block - 1));
            if (*p == all) {
                do { ++p; c += 8; } while (--left && end - c >= 8 && *p == all);
                continue;
            }
        }
        if (used(c) != state) return c;
        ++c;
    }
    return end;
}

/* the lowest run of n free clusters from c on; 0: none */
static ofs_u32 first_fit(ofs_u32 n, ofs_u32 c)
{
    ofs_u32 end = nclu + 2, b;
    for (;; c = b) {
        if ((c = skip(c, end, 1)) >= end) return 0;
        b = skip(c, end, 0);
        if (b - c >= n) return c;
    }
}

/* n free clusters at the end of the highest free run from c on that holds them; 0: none */
static ofs_u32 last_fit(ofs_u32 n, ofs_u32 c)
{
    ofs_u32 end = nclu + 2, b, t = 0;
    for (;; c = b) {
        if ((c = skip(c, end, 1)) >= end) return t;
        b = skip(c, end, 0);
        if (b - c >= n) t = b - n;
    }
}

/* the first position k of the sorted list (n entries) whose file starts at
   first or later */
static unsigned pos_of(ofs_u32 first, unsigned n)
{
    unsigned lo = 0, mid;
    while (lo < n) {
        mid = lo + (n - lo) / 2;
        if (item(D->order[mid])->first < first) lo = mid + 1; else n = mid;
    }
    return lo;
}

/* the movable file that holds cluster c, NONE */
static ofs_u32 owner(ofs_u32 c)
{
    unsigned k = pos_of(c + 1, nitems);
    struct item FAR *it;
    if (!k) return NONE;
    it = item(D->order[k - 1]);
    if (it->flags & (F_FRAG | F_STUCK) || c >= it->first + it->count) return NONE;
    return D->order[k - 1];
}

/* file i starts at first now: its place in the sorted list */
static void resort(ofs_u32 i, ofs_u32 first)
{
    unsigned k = pos_of(item(i)->first, nitems);
    while (k < nitems && D->order[k] != i) ++k;     /* equal first clusters: none, but be sure */
    FMEMMOVE(D->order + k, D->order + k + 1, (nitems - k - 1) * sizeof(ofs_defrag_idx));
    item(i)->first = first;
    k = pos_of(first, nitems - 1);
    FMEMMOVE(D->order + k + 1, D->order + k, (nitems - 1 - k) * sizeof(ofs_defrag_idx));
    D->order[k] = (ofs_defrag_idx)i;
}

static void sort(void)
{
    unsigned gap, i, j;
    ofs_defrag_idx v;
    ofs_u32 f;
    for (i = 0; i < nitems; ++i) D->order[i] = (ofs_defrag_idx)i;
    for (gap = nitems / 2; gap; gap /= 2)
        for (i = gap; i < nitems; ++i) {
            v = D->order[i]; f = item(v)->first;
            for (j = i; j >= gap && item(D->order[j - gap])->first > f; j -= gap) D->order[j] = D->order[j - gap];
            D->order[j] = v;
        }
}

/* ---- the directory tree ---- */

/* the directory of item parent (NONE: the root) */
static void dir_of(struct ofs_exfat_file *d, ofs_u32 parent)
{
    struct item FAR *p;
    if (parent == NONE) { ofs_exfat_rootdir(&V, d); return; }
    p = item(parent);
    memset(d, 0, sizeof(*d));
    d->first = p->first;
    d->size = (ofs_fsize)p->count << cb;
    d->nofat = (ofs_u8)(p->flags & F_NOFAT ? 1 : 0);
    d->root = (ofs_u8)!d->nofat;        /* a chained directory: as long as its chain */
}

/* clusters of a file and whether its chain is fragmented */
static int chain(const struct ofs_exfat_file *f, ofs_u32 *count, int *frag)
{
    ofs_u32 n, c, v, k;
    int r;
    n = (ofs_u32)(f->size >> cb) + ((f->size & (((ofs_fsize)1 << cb) - 1)) != 0);
    *count = n; *frag = 0;
    if (!n || f->first + n > nclu + 2 || f->first < 2) return n ? OFS_DEFRAG_EDIR : 0;
    if (f->nofat) return 0;
    for (c = f->first, k = 1; ; ++k) {
        if ((r = ofs_exfat_next(&V, c, &v)) != 0) return err(r);
        if (k == n) return v == 0xFFFFFFFFUL ? 0 : OFS_DEFRAG_EDIR;
        if (v < 2 || v >= nclu + 2) return OFS_DEFRAG_EDIR;
        if (v != c + 1) *frag = 1;
        c = v;
    }
}

/* the files and subdirectories of a directory, then those of the subdirectories */
static int walk(ofs_u32 parent, unsigned depth)
{
    struct ofs_exfat_file d;
    struct item FAR *it;
    ofs_u32 pos = 0, count, from = nitems, to, i;
    int r, frag, stuck;
    if (depth > MAXDEPTH) return OFS_DEFRAG_EDIR;
    dir_of(&d, parent);
    for (;;) {
        r = ofs_exfat_dir_next(&V, &d, &pos, &ent);
        if (r == OFS_EXFAT_END) break;
        if (r) return err(r);
        if (ent.file.first < 2) continue;                   /* empty: no clusters */
        if (nitems >= D->maxitems || (ofs_defrag_idx)(nitems + 1) == 0 ||
            bmsize + (nitems + 1UL) * ISIZE > D->memsize) return OFS_DEFRAG_EMEM;
        if ((r = chain(&ent.file, &count, &frag)) != 0) return r;
        if (!count) continue;
        stuck = !ofs_exfat_movable(&V, &d, ent.pos);
        it = item(nitems++);
        it->first = ent.file.first; it->count = count; it->parent = parent; it->pos = ent.pos;
        it->flags = (ofs_u8)((ent.attr & OFS_EXFAT_DIR ? F_DIR : 0) | (frag ? F_FRAG : 0) |
                             (ent.file.nofat ? F_NOFAT : 0) | (stuck ? F_STUCK : 0));
        if (ent.attr & OFS_EXFAT_DIR) ++D->dirs; else ++D->files;
        if (frag) ++D->fragmented;
    }
    for (to = nitems, i = from; i < to; ++i)
        if (item(i)->flags & F_DIR && (r = walk(i, depth + 1)) != 0) return r;
    return 0;
}

/* ---- moving a file ---- */

/* the clusters of the chain at first (n of them) as free in the map */
static int release(ofs_u32 first, ofs_u32 n, int nofat)
{
    ofs_u32 c = first, v = 0, k, run = 0;
    int r;
    for (k = 0; k < n; ++k, c = v) {
        if (nofat) v = c + 1;
        else if (k + 1 < n && (r = ofs_exfat_next(&V, c, &v)) != 0) return err(r);
        set_used(c, 1, 0);
        if (k + 1 == n || v != c + 1) { show(c - run, run + 1, OFS_DEFRAG_FREE); run = 0; }
        else ++run;
    }
    return 0;
}

static int move(ofs_u32 i, ofs_u32 t)
{
    struct item FAR *it = item(i);
    struct ofs_exfat_file d;
    ofs_u32 first = it->first, n = it->count, p;
    unsigned c;
    int r;
    dir_of(&d, it->parent);
    show(first, n, OFS_DEFRAG_READ);
    if ((r = ofs_exfat_relocate(&V, &d, it->pos, first, t)) != 0) return err(r);
    set_used(t, n, 1);
    show(t, n, OFS_DEFRAG_WRITE);
    /* the old chain is still in the FAT: freed in the map */
    if ((r = release(first, n, it->flags & F_NOFAT)) != 0) return r;
    resort(i, t);
    it->flags &= (ofs_u8)~F_FRAG;
    ++D->moves; D->moved += n;
    show(t, n, OFS_DEFRAG_USED);
    /* TexFAT: the parent's clusters it copied -- the parent is fragmented now */
    for (c = 0; c < V.ncow; ++c) {
        set_used(V.cow[c][0], 1, 0);
        set_used(V.cow[c][1], 1, 1);
        show(V.cow[c][0], 1, OFS_DEFRAG_FREE);
        show(V.cow[c][1], 1, OFS_DEFRAG_USED);
        if ((p = it->parent) != NONE) item(p)->flags |= F_FRAG;
    }
    return 0;
}

/* every fragmented file to the lowest free run that holds it, or with
   to_end to the highest (FULL places them after) */
static int unfragment(int to_end)
{
    struct item FAR *it;
    ofs_u32 i, t, left = D->fragmented;
    int r;
    for (i = 0; i < nitems; ++i) {
        it = item(i);
        if ((it->flags & (F_FRAG | F_STUCK)) != F_FRAG) continue;
        say("Joining fragmented files: %lu left", (unsigned long)(left ? left-- : 0));
        if (!(t = to_end ? last_fit(it->count, 2) : first_fit(it->count, 2))) continue;
        if (stopped()) return OFS_DEFRAG_ESTOP;
        if ((r = move(i, t)) != 0) return r;
    }
    return 0;
}

/* the directories fragmented again (TexFAT), the deepest first */
static int rejoin(void)
{
    struct item FAR *it;
    ofs_u32 i, t;
    int r;
    for (i = nitems; i--; ) {
        it = item(i);
        if ((it->flags & (F_FRAG | F_STUCK)) != F_FRAG) continue;
        say("Joining directories again");
        if (!(t = first_fit(it->count, 2))) continue;
        if (stopped()) return OFS_DEFRAG_ESTOP;
        if ((r = move(i, t)) != 0) return r;
    }
    return 0;
}

static int full(void)
{
    struct item FAR *it;
    ofs_u32 p = 2, c, n, t, k, o;
    unsigned pass, placed = 0;
    int r;
    for (pass = 0; pass < 2; ++pass)    /* directories, then files */
        for (k = 0; k < nitems; ++k) {
            it = item(k);
            if ((it->flags & F_DIR ? 0u : 1u) != pass || it->flags & F_STUCK) continue;
            n = it->count;
            say("Placing %s %u of %u", pass ? "files" : "directories", ++placed, nitems);
            for (;;) {
                D->done = p;
                if (it->first == p && !(it->flags & F_FRAG)) { p += n; break; }
                if (p + n > nclu + 2) return 0;
                if ((c = skip(p, p + n, 0)) == p + n) {
                    if (stopped()) return OFS_DEFRAG_ESTOP;
                    if ((r = move(k, p)) != 0) return r;
                    p += n;
                    break;
                }
                if ((o = owner(c)) == NONE) { p = c + 1; continue; }   /* stays where it is */
                /* the file in the way (or this one) to the end */
                if (!(t = last_fit(item(o)->count, p + n))) {
                    say("Not enough free space to go on");
                    return 0;
                }
                if (stopped()) return OFS_DEFRAG_ESTOP;
                if ((r = move(o, t)) != 0) return r;
            }
        }
    D->done = p;
    return 0;
}

int ofs_exdefrag_run(struct ofs_exdefrag *d)
{
    struct item FAR *it;
    ofs_u32 c, off, len, i;
    int r, u, rc;
    D = d;
    d->tex = 0;
    d->used = d->files = d->dirs = d->fragmented = d->fragmented_after = d->stuck = 0;
    d->moves = d->moved = d->done = 0;
    nitems = 0;
    if ((r = ofs_exfat_mount(&V, d->read, d->dev, d->start, NULL, d->buf)) != 0)
        return r == OFS_EXFAT_EIO ? OFS_DEFRAG_EIO : OFS_DEFRAG_EBAD;
    if (d->mode != OFS_DEFRAG_ANALYZE) {
        if (V.dirty) return OFS_DEFRAG_EDIR;            /* not cleanly unmounted: check it first */
        V.write = d->write;
    }
    nclu = V.clusters;
    cb = (unsigned)V.sshift + V.cshift;
    d->tex = V.tex;
    d->clusters = nclu;
    d->cluster_size = 1UL << cb;
    bmsize = ((nclu + 2 + 7) / 8 + ISIZE - 1) & ~(ofs_u32)(ISIZE - 1);
    if (bmsize > d->memsize) return OFS_DEFRAG_EMEM;
    for (off = 0; off < bmsize; off += len) {
        len = d->block - (off & (d->block - 1));
        if (len > bmsize - off) len = bmsize - off;
        FMEMSET(at(off), 0, (unsigned)len);
    }
    say("Reading the allocation bitmap");
    for (c = 2; c < nclu + 2; ++c) {
        if ((r = ofs_exfat_used(&V, c, &u)) != 0) return err(r);
        if (u) { set_used(c, 1, 1); ++d->used; }
    }
    show(2, nclu, OFS_DEFRAG_USED);
    say("Reading the directories");
    if ((r = walk(NONE, 0)) != 0) return r;
    sort();
    for (i = 0; i < nitems; ++i)
        if ((item(i)->flags & (F_FRAG | F_STUCK)) == (F_FRAG | F_STUCK)) ++d->stuck;
    if (d->mode == OFS_DEFRAG_ANALYZE) { d->fragmented_after = d->fragmented; return 0; }
    rc = unfragment(d->mode == OFS_DEFRAG_FULL);
    if (!rc && d->mode == OFS_DEFRAG_FULL) rc = full();
    if (!rc) rc = rejoin();
    if (rc == OFS_DEFRAG_EIO || rc == OFS_DEFRAG_EDIR) return rc;
    if ((r = ofs_exfat_clean(&V)) != 0) return err(r);  /* VolumeDirty off; TexFAT: nothing left open */
    for (i = 0; i < nitems; ++i) {
        it = item(i);
        if (it->flags & F_FRAG) ++d->fragmented_after;
    }
    return rc;
}
#endif
