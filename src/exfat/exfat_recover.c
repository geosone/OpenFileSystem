/* exFAT recovery; what it does is described in include/ofs/fat_recover.h.
   It only reads the volume.  Two bit maps (work memory): got -- the
   clusters already saved or walked as a directory (a loop or a cross-link
   is not followed twice); mark -- during the scan the directory clusters
   reached from a lost directory (those are not roots of their own), then
   the clusters some FAT entry points to (for the heads of lost chains).
   After them the first clusters of the directories the scan found. */
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include "ofs/fat_recover.h"

#define FAR OFS_FATREC_FAR
#ifdef __WATCOMC__
#define FMEMSET _fmemset
#else
#define FMEMSET memset
#endif
#define MAXDEPTH 64
#define MAXENT   0x800000UL              /* entries of a directory: exFAT's 256 MiB */
#define NOSEC    ((ofs_lba)-1)
#define EOC      0xFFFFFFFFUL
#define LOST     "\\LOST"
#define DELETED  "\\DELETED"
#define PATHMAX  1024

static struct ofs_fatrec *R;
static ofs_u8 *dbuf, *fbuf;              /* a directory or data sector; a FAT or bitmap sector */
static ofs_lba dsec, fsec;
static unsigned nfats, active, ssize, sshift, cshift;
static ofs_lba fat0, heap;
static ofs_u32 fatlen, nclu, rootclu, bmsize, ncand, lost_no;
static ofs_u32 bmp_first;                /* the allocation bitmap, 0: not found */
static ofs_fsize bmp_len;
static int dry;                          /* walking lost directories to mark, not to save */
static char text[PATHMAX + 120];
static char path[PATHMAX];

static ofs_u16 rd16(const ofs_u8 *p) { return (ofs_u16)(p[0] | p[1] << 8); }
static ofs_u32 rd32(const ofs_u8 *p) { return rd16(p) | (ofs_u32)rd16(p + 2) << 16; }
#ifdef OFS_NARROW
/* sizes of 4 GB and more cannot be saved here: the low part, flagged */
static ofs_fsize rd64(const ofs_u8 *p) { return rd32(p + 4) ? 0xFFFFFFFFUL : rd32(p); }
#else
static ofs_fsize rd64(const ofs_u8 *p) { return rd32(p) | (ofs_fsize)rd32(p + 4) << 32; }
#endif

static void say(int level, const char *fmt, ...)
{
    va_list a;
    if (!R->msg) return;
    va_start(a, fmt);
    vsprintf(text, fmt, a);
    va_end(a);
    R->msg(R->ctx, level, text);
}

static int stopped(void) { return R->stop && R->stop(R->ctx); }
static int valid(ofs_u32 c) { return c >= 2 && c < nclu + 2; }

/* ---- work memory ---- */

static ofs_u8 FAR *at(ofs_u32 off) { return (ofs_u8 FAR *)R->mem(R->ctx, off); }
static int bit(ofs_u32 base, ofs_u32 c) { return *at(base + (c >> 3)) >> (unsigned)(c & 7) & 1; }
static void set(ofs_u32 base, ofs_u32 c) { *at(base + (c >> 3)) |= (ofs_u8)(1 << (unsigned)(c & 7)); }
#define GOT 0
#define MARK bmsize
static ofs_u32 FAR *cand(ofs_u32 i) { return (ofs_u32 FAR *)at(2 * bmsize + i * 4); }

static void clear(ofs_u32 base)
{
    ofs_u32 off, len;
    for (off = 0; off < bmsize; off += len) {
        len = R->block - ((base + off) & (R->block - 1));
        if (len > bmsize - off) len = bmsize - off;
        FMEMSET(at(base + off), 0, (unsigned)len);
    }
}

/* a directory's cluster: walked (saving) or reached (the dry walk) */
static int seen(ofs_u32 c) { return bit(dry ? MARK : GOT, c); }
static void see(ofs_u32 c) { set(dry ? MARK : GOT, c); }

/* ---- sectors ---- */

static int rsec(ofs_u8 *buf, ofs_lba *cached, ofs_lba lba)
{
    if (*cached == lba) return 0;
    *cached = NOSEC;
    if (R->read(R->dev, R->start + lba, buf)) return -1;
    *cached = lba;
    return 0;
}

static ofs_lba clu_lba(ofs_u32 c) { return heap + ((ofs_lba)(c - 2) << cshift); }

/* ---- the FAT ---- */

static int raw_get(unsigned k, ofs_u32 c, ofs_u32 *v)
{
    ofs_lba lba = fat0 + (ofs_lba)k * fatlen + (((ofs_lba)c * 4) >> sshift);
    if (rsec(fbuf, &fsec, lba)) return -1;
    *v = rd32(fbuf + (unsigned)((c * 4) & (ssize - 1)));
    return 0;
}

static int links(ofs_u32 v) { return valid(v) || v == EOC; }

/* c's next cluster by the first copy that links it (the active first),
   else what a readable copy says, 0 when none can be read */
static ofs_u32 next(ofs_u32 c)
{
    ofs_u32 v, first = 0;
    int any = 0;
    unsigned i;
    for (i = 0; i < nfats; ++i) {
        if (raw_get((active + i) % nfats, c, &v)) continue;
        if (links(v)) return v;
        if (!any) first = v, any = 1;
    }
    return first;
}

/* the allocation bitmap: 1 used, 0 free, -1 unknown */
static int allocated(ofs_u32 c)
{
    ofs_u32 b = c - 2, k = b >> (sshift + 3);
    if (!bmp_first || ((ofs_fsize)b >> 3) >= bmp_len) return -1;
    if (k >> cshift >= nclu || !valid(bmp_first + (k >> cshift))) return -1;
    if (rsec(fbuf, &fsec, clu_lba(bmp_first + (k >> cshift)) + (k & ((1UL << cshift) - 1)))) return -1;
    return fbuf[(b >> 3) & (ssize - 1)] >> (unsigned)(b & 7) & 1;
}

/* ---- names ---- */

static void put_utf8(char *out, unsigned *n, unsigned max, ofs_u16 u)
{
    if (u < 0x20 || u == '\\' || u == '/' || u == ':' || u == '*' || u == '?' || u == '"' || u == '<' ||
        u == '>' || u == '|')
        u = '_';
    if (u < 0x80) { if (*n + 1 < max) out[(*n)++] = (char)u; }
    else if (u < 0x800) {
        if (*n + 2 < max) { out[(*n)++] = (char)(0xC0 | u >> 6); out[(*n)++] = (char)(0x80 | (u & 0x3F)); }
    } else if (*n + 3 < max) {
        out[(*n)++] = (char)(0xE0 | u >> 12); out[(*n)++] = (char)(0x80 | (u >> 6 & 0x3F));
        out[(*n)++] = (char)(0x80 | (u & 0x3F));
    }
}

/* ---- directories ---- */

struct dir {
    ofs_u32 first;
    ofs_u32 ncl;                         /* NoFatChain: its clusters; 0: a FAT chain */
    ofs_u32 idx, clu;                    /* the cluster idx of it: clu */
};

/* the set's checksum; it was made with the in-use bits set, which a
   deletion clears */
static int set_sum_ok(const ofs_u8 *set, unsigned n)
{
    ofs_u16 s = 0;
    unsigned i;
    for (i = 0; i < n * 32; ++i)
        if (i != 2 && i != 3)
            s = (ofs_u16)(((s & 1) ? 0x8000u : 0) + (s >> 1) + (set[i] | (i & 31 ? 0 : 0x80)));
    return s == rd16(set + 2);
}

/* the first sector of cluster c begins with a whole file entry set of a
   right checksum */
static int set_starts(ofs_u32 c)
{
    unsigned sc;
    if (rsec(dbuf, &dsec, clu_lba(c))) return 0;
    sc = dbuf[1];
    if (dbuf[0] != 0x85 || sc < 2 || sc > 18 || (sc + 1) * 32u > ssize || dbuf[32] != 0xC0 || dbuf[64] != 0xC1)
        return 0;
    return set_sum_ok(dbuf, sc + 1);
}

/* the first sector of cluster c looks like exFAT directory entries: for a
   directory whose chain breaks, as the cluster after it */
static int plausible(ofs_u32 c)
{
    unsigned i, k, used = 0;
    ofs_u8 t;
    if (!valid(c) || bit(GOT, c) || (dry && bit(MARK, c)) || rsec(dbuf, &dsec, clu_lba(c))) return 0;
    for (i = 0; i < ssize; i += 32) {
        t = dbuf[i];
        if (!t) {
            for (k = i; k < ssize; ++k) if (dbuf[k]) return 0;
            break;
        }
        ++used;
        if (!(t & 0x80)) continue;       /* unused or deleted */
        if (t != 0x81 && t != 0x82 && t != 0x83 && t != 0x85 && t != 0xA0 && t != 0xA1 && t != 0xA2 && t != 0xC0 &&
            t != 0xC1 && t != 0xE0)
            return 0;
    }
    return used != 0;
}

/* the sector of entry pos; NOSEC past the end */
static ofs_lba entry_lba(struct dir *d, ofs_u32 pos)
{
    ofs_u32 byte = pos * 32, ci = byte >> (sshift + cshift), v;
    if (d->ncl) {
        if (ci >= d->ncl || !valid(d->first + ci)) return NOSEC;
        return clu_lba(d->first + ci) + ((byte >> sshift) & ((1UL << cshift) - 1));
    }
    if (ci < d->idx) d->idx = 0, d->clu = d->first;
    while (d->idx < ci) {
        v = next(d->clu);
        if (!valid(v)) {
            if (v != EOC && plausible(d->clu + 1)) {
                v = d->clu + 1;
                see(v);
                if (!dry)
                    say(OFS_FATREC_WARN, "%s: the directory's chain breaks; cluster %lu taken as its next",
                        path[0] ? path : "\\", (unsigned long)v);
            } else {
                return NOSEC;
            }
        }
        d->clu = v, ++d->idx;
    }
    return clu_lba(d->clu) + ((byte >> sshift) & ((1UL << cshift) - 1));
}

/* entry pos into e: 1, 0 at the end, -1 unreadable */
static int entry(struct dir *d, ofs_u32 pos, ofs_u8 *e)
{
    ofs_lba lba;
    if (pos >= MAXENT || (lba = entry_lba(d, pos)) == NOSEC) return 0;
    if (rsec(dbuf, &dsec, lba)) return -1;
    memcpy(e, dbuf + ((pos * 32) & (ssize - 1)), 32);
    return 1;
}

static void walk(ofs_u32 first, ofs_u32 ncl, unsigned depth);

/* a file to the target: its clusters (contiguous with NoFatChain), the
   bytes past valid as zeros.  A FAT chain that breaks goes on with the
   next cluster the bitmap marks used and nothing took yet. */
/* cluster v free for a deleted file (del), else one in use: not taken by
   anything saved, and the bitmap not saying otherwise */
static int fits(ofs_u32 v, int del)
{
    return valid(v) && !bit(GOT, v) && (del ? allocated(v) != 1 : allocated(v) != 0);
}

static void make_dirs(void);

static void save_file(ofs_u32 first, int nofat, ofs_fsize size, ofs_fsize vdl, ofs_u16 time, ofs_u16 date, int del)
{
    ofs_u32 csize = 1UL << (sshift + cshift), n = size ? (ofs_u32)((size - 1) / csize + 1) : 0, k, c = first, v;
    ofs_fsize left = size, off = 0;
    ofs_lba lba;
    unsigned j, len, z;
    int guessed = 0, failed = 0;
    if (del) {
        if (n && !fits(first, 1)) {
            ++R->overwritten;
            say(OFS_FATREC_WARN, "%s: deleted, and its first cluster %lu is in use again: left out", path,
                (unsigned long)first);
            return;
        }
        make_dirs();
    }
    if (R->create(R->ctx, path, size, time, date)) {
        ++R->failed;
        say(OFS_FATREC_ERROR, "%s: cannot be made on the target", path);
        return;
    }
    if (n && !valid(first)) {
        say(OFS_FATREC_WARN, "%s: its first cluster is outside the volume, saved empty", path);
        n = 0;
    }
    for (k = 0; k < n && !failed; ++k) {
        if (k) {
            v = nofat ? c + 1 : next(c);
            /* a deleted file's clusters are free (its FAT chain is left,
               the bitmap cleared); one in use again is skipped */
            if (del ? !fits(v, 1) : !nofat && (!valid(v) || bit(GOT, v))) {
                if (!guessed)
                    say(OFS_FATREC_WARN, del ? "%s: deleted, its clusters after %lu of %lu are in use again; the rest taken from the free ones after"
                                             : "%s: its chain breaks after %lu of %lu clusters; the rest taken from the clusters after",
                        path, (unsigned long)k, (unsigned long)n);
                guessed = 1;
                /* the next cluster after it that the bitmap marks free
                   (deleted) or used, and nothing else took */
                for (v = c + 1; valid(v) && !fits(v, del); ++v) ;
            }
            if (!valid(v)) {
                say(OFS_FATREC_WARN, "%s: cut at the end of the volume", path);
                break;
            }
            c = v;
        }
        set(GOT, c);
        lba = clu_lba(c);
        for (j = 0; j < 1u << cshift && left; ++j) {
            len = left < ssize ? (unsigned)left : ssize;
            if (off >= vdl) {
                memset(dbuf, 0, ssize);
                dsec = NOSEC;
            } else if (rsec(dbuf, &dsec, lba + j)) {
                memset(dbuf, 0, ssize);
                ++R->bad;
                say(OFS_FATREC_WARN, "%s: sector %lu unreadable, saved as zeros", path, (unsigned long)(lba + j));
            } else if (vdl - off < len) {
                z = (unsigned)(vdl - off);
                memset(dbuf + z, 0, ssize - z);
                dsec = NOSEC;
            }
            if (R->write(R->ctx, dbuf, len)) { failed = 1; break; }
            left -= len, off += len;
            if (R->bytes_lo + len < R->bytes_lo) ++R->bytes_hi;
            R->bytes_lo += len;
        }
    }
    dsec = NOSEC;
    if (R->close(R->ctx) || failed) {
        ++R->failed;
        say(OFS_FATREC_ERROR, "%s: writing it on the target failed", path);
        return;
    }
    if (del) ++R->undeleted;
    else ++R->files;
    if (guessed) ++R->guessed;
}

/* the clusters of a contiguous system file (bitmap, up-case table) */
static void claim(ofs_u32 first, ofs_fsize len)
{
    ofs_u32 csize = 1UL << (sshift + cshift), n = len ? (ofs_u32)((len - 1) / csize + 1) : 0, k;
    for (k = 0; k < n && valid(first + k); ++k) set(GOT, first + k);
}

/* the entries of a directory; path is its path on the target */
static void walk_dir(struct dir *d, unsigned depth)
{
    static ofs_u8 set_[19 * 32];         /* not needed past the recursion: static (DOS stacks) */
    ofs_u8 *e = set_;
    ofs_u32 pos, sub, ncl, csize = 1UL << (sshift + cshift);
    unsigned plen = (unsigned)strlen(path), n, i, k, sc, nlen;
    ofs_fsize size, vdl;
    int r, ok, nofat;
    for (pos = 0;; ++pos) {
        if (stopped()) return;
        r = entry(d, pos, e);
        if (!r) return;
        if (r < 0) {
            if (!dry) {
                ++R->bad;
                say(OFS_FATREC_WARN, "%s: a directory sector is unreadable", plen ? path : "\\");
            }
            pos |= (ssize / 32) - 1;
            continue;
        }
        if (!e[0]) return;
        if (!(e[0] & 0x80)) continue;
        if (e[0] == 0x81 && !dry && !depth) {        /* the allocation bitmap (TexFAT: the active one) */
            if (!bmp_first || (e[1] & 1) == active) bmp_first = rd32(e + 20), bmp_len = rd64(e + 24);
            claim(rd32(e + 20), rd64(e + 24));
            continue;
        }
        if (e[0] == 0x82 && !dry && !depth) { claim(rd32(e + 20), rd64(e + 24)); continue; }
        if (e[0] != 0x85) continue;
        /* the set */
        sc = e[1];
        if (sc < 2 || sc > 18) continue;
        for (k = 1; k <= sc; ++k)
            if (entry(d, pos + k, e + k * 32) != 1) break;
        if (k <= sc || e[32] != 0xC0) continue;      /* not whole: the next entry */
        for (k = 2; k <= sc && e[k * 32] == 0xC1; ++k) ;
        nlen = e[35];
        if (k == 2 || !nlen || (nlen + 14) / 15 > k - 2) continue;
        ok = set_sum_ok(e, sc + 1);
        pos += sc;
        n = plen;
        if (n + 2 >= sizeof(path)) return;
        path[n++] = '\\';
        for (i = 0; i < nlen; ++i) put_utf8(path, &n, sizeof(path), rd16(e + 64 + (i / 15) * 32 + 2 + (i % 15) * 2));
        path[n] = 0;
        if (!ok && !dry) say(OFS_FATREC_WARN, "%s: its entry checksum is wrong; taken as it reads", path);
        nofat = e[33] & 2;
        sub = e[33] & 1 ? rd32(e + 52) : 0;
        size = rd64(e + 56);
        vdl = rd64(e + 40);
        if (vdl > size) vdl = size;
        if (e[4] & 0x10) {
            ncl = nofat ? (size ? (ofs_u32)((size - 1) / csize + 1) : 0) : 0;
            if (!sub || !size) {
                if (!dry) {
                    if (R->mkdir(R->ctx, path)) ++R->failed;
                    else ++R->dirs;
                }
            } else if (!valid(sub)) {
                if (!dry) say(OFS_FATREC_WARN, "%s: a directory outside the volume, left out", path);
            } else if (bit(GOT, sub) || seen(sub)) {
                if (!dry) say(OFS_FATREC_WARN, "%s: a directory seen before (a loop), left out", path);
            } else if (depth >= MAXDEPTH) {
                if (!dry) say(OFS_FATREC_WARN, "%s: too deep, left out", path);
            } else {
                walk(sub, ncl, depth + 1);
            }
        } else if (!dry) {
            save_file(sub, nofat, sub ? size : 0, sub ? vdl : 0, rd16(e + 12), rd16(e + 14), 0);
        }
        path[plen] = 0;
        dsec = NOSEC;
    }
}

/* a directory at its first cluster (ncl: NoFatChain, its clusters) */
static void walk(ofs_u32 first, ofs_u32 ncl, unsigned depth)
{
    struct dir d;
    ofs_u32 c, k;
    if (!dry) {
        if (R->mkdir(R->ctx, path)) {
            ++R->failed;
            say(OFS_FATREC_ERROR, "%s: cannot be made on the target", path);
            return;
        }
        ++R->dirs;
    }
    if (ncl) {
        for (k = 0; k < ncl && valid(first + k); ++k) see(first + k);
    } else {
        for (c = first, k = 0; valid(c) && !seen(c) && k < MAXENT * 32 / (1UL << (sshift + cshift)) + 1; ++k) {
            see(c);
            c = next(c);
        }
    }
    d.first = first, d.ncl = ncl, d.idx = 0, d.clu = first;
    walk_dir(&d, depth);
}

/* ---- deleted files ---- */

/* the directories above path on the target (they may be there already) */
static void make_dirs(void)
{
    unsigned i;
    char c;
    for (i = 1; path[i]; ++i) {
        if (path[i] != '\\') continue;
        c = path[i];
        path[i] = 0;
        R->mkdir(R->ctx, path);
        path[i] = c;
    }
}

/* the first sector of cluster c holds directory entries only (an emptied
   directory: deleted ones, or none) */
static int dir_like(ofs_u32 c)
{
    unsigned i, k;
    ofs_u8 t;
    if (rsec(dbuf, &dsec, clu_lba(c))) return 0;
    for (i = 0; i < ssize; i += 32) {
        if (!(t = dbuf[i])) {
            for (k = i; k < ssize; ++k) if (dbuf[k]) return 0;
            return 1;
        }
        t &= 0x7F;
        if (t != 0x01 && t != 0x02 && t != 0x03 && t != 0x05 && t != 0x20 && t != 0x21 && t != 0x22 && t != 0x40 &&
            t != 0x41 && t != 0x60)
            return 0;
    }
    return 1;
}

static void walk_del(ofs_u32 first, ofs_u32 ncl, ofs_u32 len, unsigned depth, int gone);

/* A directory's deleted entry sets (gone: the directory was deleted
   itself, all its sets are), to \DELETED with the directory's path.  A
   deletion clears the in-use bit of each entry of the set (85h C0h C1h
   become 05h 40h 41h) and the clusters in the bitmap; the FAT chain and
   the name stay.  A deleted set is taken when it is whole (no entry of it
   used again) and its checksum fits. */
static void walk_dir_del(struct dir *d, unsigned depth, int gone)
{
    static ofs_u8 set_[19 * 32];
    ofs_u8 *e = set_, in;
    ofs_u32 pos, sub, ncl, csize = 1UL << (sshift + cshift);
    unsigned plen = (unsigned)strlen(path), n, i, k, sc, nlen;
    ofs_fsize size, vdl;
    int r, del, nofat;
    for (pos = 0;; ++pos) {
        if (stopped()) return;
        r = entry(d, pos, e);
        if (!r) return;
        if (r < 0) { pos |= (ssize / 32) - 1; continue; }
        if (!e[0]) return;
        if ((e[0] & 0x7F) != 0x05) continue;
        in = e[0] & 0x80;
        del = !in || gone;
        sc = e[1];
        if (sc < 2 || sc > 18) continue;
        for (k = 1; k <= sc; ++k)
            if (entry(d, pos + k, e + k * 32) != 1 || (e[k * 32] & 0x80) != in) break;
        if (k <= sc || (e[32] & 0x7F) != 0x40) continue;
        for (k = 2; k <= sc && (e[k * 32] & 0x7F) == 0x41; ++k) ;
        nlen = e[35];
        if (k == 2 || !nlen || (nlen + 14) / 15 > k - 2) continue;
        if (!in && !set_sum_ok(e, sc + 1)) continue;
        pos += sc;
        nofat = e[33] & 2;
        sub = e[33] & 1 ? rd32(e + 52) : 0;
        size = rd64(e + 56);
        vdl = rd64(e + 40);
        if (vdl > size) vdl = size;
        ncl = nofat ? (size ? (ofs_u32)((size - 1) / csize + 1) : 0) : 0;
        if (!del && (!(e[4] & 0x10) || !valid(sub) || bit(MARK, sub) || depth >= MAXDEPTH)) continue;
        n = plen;
        if (n + 2 >= sizeof(path)) return;
        path[n++] = '\\';
        for (i = 0; i < nlen; ++i) put_utf8(path, &n, sizeof(path), rd16(e + 64 + (i / 15) * 32 + 2 + (i % 15) * 2));
        path[n] = 0;
        if (!del) {                      /* a live directory: for what is deleted below */
            walk_del(sub, ncl, 0, depth + 1, 0);
        } else if (e[4] & 0x10) {
            if (!sub || !size) {
                make_dirs();
                if (R->mkdir(R->ctx, path)) ++R->failed;
                else ++R->undeleted;
            } else if (!fits(sub, 1) || !dir_like(sub)) {
                say(OFS_FATREC_WARN, "%s: a deleted directory whose cluster is in use again, left out", path);
                ++R->overwritten;
            } else if (depth >= MAXDEPTH) {
                say(OFS_FATREC_WARN, "%s: too deep, left out", path);
            } else {
                make_dirs();
                walk_del(sub, ncl, (ofs_u32)((size - 1) / csize + 1), depth + 1, 1);
            }
        } else {
            save_file(sub, nofat, sub ? size : 0, sub ? vdl : 0, rd16(e + 12), rd16(e + 14), 1);
        }
        path[plen] = 0;
        dsec = NOSEC;
    }
}

/* a directory in the pass for deleted sets.  A live one is marked (MARK,
   against loops); a deleted one (gone) is made under \DELETED and its len
   clusters taken (GOT): its FAT chain while the clusters are free, else
   the free ones after it */
static void walk_del(ofs_u32 first, ofs_u32 ncl, ofs_u32 len, unsigned depth, int gone)
{
    struct dir d;
    ofs_u32 c, k, v;
    if (gone) {
        if (R->mkdir(R->ctx, path)) {
            ++R->failed;
            say(OFS_FATREC_ERROR, "%s: cannot be made on the target", path);
            return;
        }
        ++R->undeleted;
        if (ncl) {
            for (k = 0; k < ncl && fits(first + k, 1); ++k) set(GOT, first + k);
            ncl = k;
        } else {
            for (c = first, k = 0; k < len; ++k) {
                set(GOT, c);
                if (k + 1 == len) break;
                v = next(c);
                if (!fits(v, 1) || !dir_like(v)) break;
                c = v;
            }
            /* no more than those (entry_lba goes on with the next cluster
               only while it looks like entries) */
            ncl = 0;
        }
    } else if (ncl) {
        for (k = 0; k < ncl && valid(first + k); ++k) set(MARK, first + k);
    } else {
        for (c = first, k = 0; valid(c) && !bit(MARK, c) && k < MAXENT * 32 / (1UL << (sshift + cshift)) + 1; ++k) {
            set(MARK, c);
            c = next(c);
        }
    }
    d.first = first, d.ncl = ncl, d.idx = 0, d.clu = first;
    walk_dir_del(&d, depth, gone);
}

static int deleted(void)
{
    say(OFS_FATREC_INFO, "Looking for deleted files");
    clear(MARK);
    strcpy(path, DELETED);
    if (valid(rootclu)) walk_del(rootclu, 0, 0, 0, 0);
    path[0] = 0;
    clear(MARK);                         /* the scan and CHAINS count with it from zero */
    return stopped() ? OFS_FATREC_ESTOP : 0;
}

/* ---- the scan for lost directories ---- */

static int scan(void)
{
    ofs_u32 c, i;
    int progress, pass;
    say(OFS_FATREC_INFO, "Searching %lu clusters for lost directories", (unsigned long)nclu);
    for (c = 2; c < nclu + 2; ++c) {
        if (!(c & 1023) && stopped()) return OFS_FATREC_ESTOP;
        if (bit(GOT, c) || !set_starts(c)) continue;
        if (2 * bmsize + (ncand + 1) * 4 > R->memsize) {
            say(OFS_FATREC_WARN, "Too many lost directories for the work memory; the rest left");
            break;
        }
        *cand(ncand++) = c;
    }
    /* the dry walk: every directory cluster reached from a candidate is
       marked (the candidate's own first cluster too, then unmarked again) */
    clear(MARK);
    dry = 1;
    for (i = 0; i < ncand; ++i) {
        c = *cand(i);
        if (bit(MARK, c)) continue;      /* reached already: walked with that one */
        path[0] = 0;
        walk(c, 0, 1);
        *at(MARK + (c >> 3)) &= (ofs_u8)~(1 << (unsigned)(c & 7));
    }
    dry = 0;
    /* the roots first (not reached from another), then what is left (loops) */
    for (pass = 0; pass < 2; ++pass)
        do {
            progress = 0;
            for (i = 0; i < ncand; ++i) {
                c = *cand(i);
                if (bit(GOT, c) || (!pass && bit(MARK, c))) continue;
                if (!lost_no++ && R->mkdir(R->ctx, LOST)) ++R->failed;
                sprintf(path, LOST "\\DIR%05lu", (unsigned long)lost_no);
                say(OFS_FATREC_INFO, "A lost directory at cluster %lu saved as %s", (unsigned long)c, path);
                ++R->lost_dirs;
                walk(c, 0, 1);
                path[0] = 0;
                progress = 1;
            }
        } while (progress && pass);
    return 0;
}

/* ---- what nothing uses ---- */

static void lost_file(ofs_u32 c, ofs_u32 n, int nofat, unsigned long *no)
{
    ofs_fsize sz = (ofs_fsize)n << (sshift + cshift);
    if (!lost_no++ && R->mkdir(R->ctx, LOST)) ++R->failed;
    sprintf(path, LOST "\\FILE%05lu.CHK", ++*no);
    ++R->lost_chains;
    say(OFS_FATREC_INFO, "Lost clusters: %lu at %lu saved as %s", (unsigned long)n, (unsigned long)c, path);
    save_file(c, nofat, sz, sz, 0, 0, 0);
    path[0] = 0;
}

static int chains(void)
{
    ofs_u32 c, v, n, x;
    unsigned long no = 0;
    clear(MARK);
    for (c = 2; c < nclu + 2; ++c)
        if (valid(v = next(c))) set(MARK, v);
    /* the FAT's chains */
    for (c = 2; c < nclu + 2; ++c) {
        if (!(c & 1023) && stopped()) return OFS_FATREC_ESTOP;
        if (bit(GOT, c) || bit(MARK, c) || !links(next(c)) || allocated(c) == 0) continue;
        for (n = 1, x = c; n < nclu && valid(v = next(x)) && !bit(GOT, v) && v != c; ++n) x = v;
        lost_file(c, n, 0, &no);
    }
    /* runs the bitmap marks used (NoFatChain files whose entries are gone) */
    if (!bmp_first) {
        say(OFS_FATREC_WARN, "The allocation bitmap was not found; only the FAT's lost chains");
        return 0;
    }
    for (c = 2; c < nclu + 2; ++c) {
        if (!(c & 1023) && stopped()) return OFS_FATREC_ESTOP;
        if (bit(GOT, c) || allocated(c) != 1) continue;
        for (n = 1; valid(c + n) && !bit(GOT, c + n) && allocated(c + n) == 1; ++n) ;
        lost_file(c, n, 1, &no);
        c += n - 1;
    }
    return 0;
}

/* ---- the boot sector ---- */

static int boot_ok(const ofs_u8 *b)
{
    return !memcmp(b + 3, "EXFAT   ", 8) && b[108] >= 9 && b[108] <= 12 && (1u << b[108]) <= OFS_FATREC_MAXSECT &&
           b[108] + b[109] <= 25 && (b[110] == 1 || b[110] == 2) && rd32(b + 80) >= 24 && rd32(b + 84) &&
           rd32(b + 88) >= rd32(b + 80) + b[110] * rd32(b + 84) && rd32(b + 92) && rd32(b + 92) <= 0xFFFFFFF5UL;
}

static int geometry(void)
{
    const ofs_u8 *b = dbuf;
    ofs_u32 maxclu;
    if (R->read(R->dev, R->start, dbuf)) return OFS_FATREC_EIO;
    if (!boot_ok(b)) {
        if (R->read(R->dev, R->start + 12, dbuf) || !boot_ok(b)) return OFS_FATREC_EBAD;
        say(OFS_FATREC_WARN, "The boot sector is not exFAT; using the backup boot region (sector 12)");
        R->from_backup = 1;
    }
    sshift = b[108], cshift = b[109], ssize = 1u << sshift;
    nfats = b[110];
    fat0 = rd32(b + 80), fatlen = rd32(b + 84), heap = rd32(b + 88);
    nclu = rd32(b + 92);
    rootclu = rd32(b + 96);
    active = nfats == 2 ? rd16(b + 106) & 1 : 0;
    maxclu = (ofs_u32)(((ofs_lba)fatlen << sshift) / 4);
    if (maxclu < nclu + 2) {
        say(OFS_FATREC_WARN, "The FAT is too small for %lu clusters; only those it holds", (unsigned long)nclu);
        nclu = maxclu - 2;
    }
    R->bits = 64, R->texfat = nfats == 2;
    R->clusters = nclu, R->cluster_size = 1UL << (sshift + cshift);
    return 0;
}

int ofs_exfatrec_run(struct ofs_fatrec *r)
{
    int rc;
    R = r;
    r->bits = 0;
    r->files = r->dirs = r->bytes_lo = r->bytes_hi = r->guessed = r->bad = 0;
    r->lost_dirs = r->lost_chains = r->failed = 0;
    r->undeleted = r->overwritten = 0;
    r->from_backup = r->texfat = r->searched = 0;
    dbuf = r->buf, fbuf = r->buf + OFS_FATREC_MAXSECT;
    dsec = fsec = NOSEC;
    ncand = lost_no = 0;
    bmp_first = 0, bmp_len = 0;
    dry = 0;
    if ((rc = geometry()) != 0) return rc;
    dsec = NOSEC;
    bmsize = ((nclu + 2 + 7) / 8 + 7) & ~(ofs_u32)7;
    if (2 * bmsize > r->memsize) return OFS_FATREC_EMEM;
    clear(GOT);
    clear(MARK);
    say(OFS_FATREC_INFO, "exFAT, %lu clusters of %lu bytes, %u FAT%s", (unsigned long)nclu,
        (unsigned long)r->cluster_size, nfats, nfats == 1 ? "" : "s");
    path[0] = 0;
    if (!valid(rootclu)) say(OFS_FATREC_WARN, "The root directory starts outside the volume");
    else walk(rootclu, 0, 0);
    if (stopped()) return OFS_FATREC_ESTOP;
    if (r->flags & OFS_FATREC_DELETED && (rc = deleted()) != 0) return rc;
    if (r->flags & OFS_FATREC_SCAN && (rc = scan()) != 0) return rc;
    if (r->flags & OFS_FATREC_CHAINS && (rc = chains()) != 0) return rc;
    return 0;
}
