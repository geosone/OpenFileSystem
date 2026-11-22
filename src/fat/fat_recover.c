/* FAT12/16/32 recovery; what it does is described in
   include/ofs/fat_recover.h.  It only reads the volume.  Two bit maps
   (work memory): got -- the clusters already saved (or walked as a
   directory), so a loop or a cross-link is not followed twice; linked --
   the clusters some FAT entry points to (for the heads of lost chains).
   After them the first clusters of the directories the scan found, with
   the cluster their ".." names. */
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
#define MAXENT   65536UL                 /* entries of a directory (FAT's limit) */
#define NOSEC    0xFFFFFFFFUL
#define LOST     "\\LOST"
#define PATHMAX  1024

static struct ofs_fatrec *R;
static ofs_u8 *dbuf, *fbuf;              /* a directory or data sector; a FAT sector */
static ofs_u32 dsec, fsec;               /* the sector in it (NOSEC: none) */
static unsigned bits, nfats, active, ssize, sshift, cshift;
static ofs_u32 fat0, fatlen, rootsec, rootsecs, heap, nclu, rootclu, bmsize, ncand, lost_no;
static char text[PATHMAX + 120];
static char path[PATHMAX];
static ofs_u16 lfn[20 * 13 + 1];

static ofs_u16 rd16(const ofs_u8 *p) { return (ofs_u16)(p[0] | p[1] << 8); }
static ofs_u32 rd32(const ofs_u8 *p) { return rd16(p) | (ofs_u32)rd16(p + 2) << 16; }

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
#define LINKED bmsize
static ofs_u32 FAR *cand(ofs_u32 i) { return (ofs_u32 FAR *)at(2 * bmsize + i * 8); }

/* ---- sectors ---- */

/* 0, or -1 when it cannot be read */
static int rsec(ofs_u8 *buf, ofs_u32 *cached, ofs_u32 lba)
{
    if (*cached == lba) return 0;
    *cached = NOSEC;
    if (R->read(R->dev, R->start + lba, buf)) return -1;
    *cached = lba;
    return 0;
}

static ofs_u32 clu_lba(ofs_u32 c) { return heap + ((c - 2) << cshift); }

/* ---- the FAT ---- */

/* entry c of copy k; -1 unreadable */
static int raw_get(unsigned k, ofs_u32 c, ofs_u32 *v)
{
    ofs_u32 base = fat0 + k * fatlen, off = bits == 12 ? c + c / 2 : c * (bits / 8);
    ofs_u32 lba = base + (off >> sshift);
    unsigned o = (unsigned)(off & (ssize - 1));
    ofs_u8 lo, hi;
    if (rsec(fbuf, &fsec, lba)) return -1;
    if (bits == 32) { *v = rd32(fbuf + o) & 0x0FFFFFFFUL; return 0; }
    if (bits == 16) { *v = rd16(fbuf + o); return 0; }
    lo = fbuf[o];
    if (o + 1 < ssize) hi = fbuf[o + 1];
    else { if (rsec(fbuf, &fsec, lba + 1)) return -1; hi = fbuf[0]; }
    *v = (ofs_u32)(lo | hi << 8);
    *v = c & 1 ? *v >> 4 : *v & 0xFFF;
    return 0;
}

static ofs_u32 eoc_min(void) { return bits == 12 ? 0xFF8 : bits == 16 ? 0xFFF8UL : 0x0FFFFFF8UL; }
static int links(ofs_u32 v) { return valid(v) || v >= eoc_min(); }

/* c's next cluster by the first copy that links it; else what a readable
   copy says (free, bad); 0 when no copy can be read */
static ofs_u32 next(ofs_u32 c)
{
    ofs_u32 v, first = 0;
    int any = 0;
    unsigned i;
    for (i = 0; i < nfats; ++i) {
        unsigned k = (active + i) % nfats;
        if (raw_get(k, c, &v)) continue;
        if (links(v)) return v;
        if (!any) first = v, any = 1;
    }
    return first;
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

static ofs_u16 oem(ofs_u8 c) { return c < 0x80 ? c : R->cp && R->cp[c - 0x80] ? R->cp[c - 0x80] : '_'; }

/* the 8.3 name of e (NT's lower case bits) */
static void short_name(const ofs_u8 *e, char *out, unsigned max)
{
    unsigned n = 0, i, end;
    ofs_u8 c;
    for (end = 8; end && e[end - 1] == ' '; --end) ;
    for (i = 0; i < end; ++i) {
        c = (ofs_u8)(i == 0 && e[0] == 0x05 ? 0xE5 : e[i]);
        if (e[12] & 0x08 && c >= 'A' && c <= 'Z') c = (ofs_u8)(c + 32);
        put_utf8(out, &n, max, oem(c));
    }
    for (end = 11; end > 8 && e[end - 1] == ' '; --end) ;
    if (end > 8) {
        put_utf8(out, &n, max, '.');
        for (i = 8; i < end; ++i) {
            c = e[i];
            if (e[12] & 0x10 && c >= 'A' && c <= 'Z') c = (ofs_u8)(c + 32);
            put_utf8(out, &n, max, oem(c));
        }
    }
    if (!n) put_utf8(out, &n, max, '_');
    out[n] = 0;
}

static ofs_u8 lfn_sum(const ofs_u8 *e)
{
    ofs_u8 s = 0;
    unsigned i;
    for (i = 0; i < 11; ++i) s = (ofs_u8)(((s & 1) << 7) + (s >> 1) + e[i]);
    return s;
}

/* ---- directories ---- */

struct dir {
    ofs_u32 first;                       /* 0: FAT12/16's fixed root */
    ofs_u32 idx, clu;                    /* the cluster idx of the chain: clu */
};

/* the first sector of cluster c looks like directory entries (and not
   like the start of another directory): for a directory whose chain
   breaks, as the clusters after it */
static int plausible(ofs_u32 c)
{
    const ofs_u8 *e;
    unsigned i, k, used = 0;
    if (!valid(c) || bit(GOT, c) || rsec(dbuf, &dsec, clu_lba(c))) return 0;
    if (!memcmp(dbuf, ".          ", 11)) return 0;
    for (i = 0; i < ssize; i += 32) {
        e = dbuf + i;
        if (e[0] == 0) {                 /* the end: zeros after it */
            for (k = i; k < ssize; ++k) if (dbuf[k]) return 0;
            break;
        }
        ++used;
        if (e[0] == 0xE5) continue;
        if ((e[11] & 0x3F) == 0x0F) {    /* a long name part */
            if ((e[0] & 0x1F) == 0 || (e[0] & 0x1F) > 20 || e[26] || e[27]) return 0;
            continue;
        }
        if (e[11] & 0xC0) return 0;
        for (k = 0; k < 11; ++k)
            if ((e[k] < 0x20 && !(k == 0 && e[k] == 0x05)) || e[k] == '"' || e[k] == '*' || e[k] == '/' ||
                e[k] == ':' || e[k] == '<' || e[k] == '>' || e[k] == '?' || e[k] == '\\' || e[k] == '|')
                return 0;
    }
    return used != 0;
}

/* the sector of entry pos; NOSEC past the end */
static ofs_u32 entry_lba(struct dir *d, ofs_u32 pos)
{
    ofs_u32 byte = pos * 32, ci, v;
    if (!d->first) return byte >> sshift < rootsecs ? rootsec + (byte >> sshift) : NOSEC;
    ci = byte >> (sshift + cshift);
    if (ci < d->idx) d->idx = 0, d->clu = d->first;
    while (d->idx < ci) {
        v = next(d->clu);
        if (!valid(v)) {
            /* a broken chain: the cluster after it, when it looks like more of the directory */
            if (v < eoc_min() && plausible(d->clu + 1)) {
                v = d->clu + 1;
                set(GOT, v);
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

static void walk(ofs_u32 first, unsigned depth);

/* the clusters of a file to the target: n, the first, the size */
static void save_file(ofs_u32 first, ofs_u32 size, ofs_u16 time, ofs_u16 date)
{
    ofs_u32 csize = 1UL << (sshift + cshift), n = size ? (size - 1) / csize + 1 : 0, k, c = first, v, left = size;
    ofs_u32 lba;
    unsigned j, len;
    int guessed = 0, failed = 0;
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
            v = next(c);
            if (!valid(v) || bit(GOT, v)) {
                if (!guessed)
                    say(OFS_FATREC_WARN, "%s: its chain breaks after %lu of %lu clusters; the rest taken from the clusters after",
                        path, (unsigned long)k, (unsigned long)n);
                guessed = 1;
                v = c + 1;
                if (!valid(v)) {
                    say(OFS_FATREC_WARN, "%s: cut at the end of the volume", path);
                    break;
                }
            }
            c = v;
        }
        set(GOT, c);
        lba = clu_lba(c);
        for (j = 0; j < 1u << cshift && left; ++j) {
            len = left < ssize ? (unsigned)left : ssize;
            if (rsec(dbuf, &dsec, lba + j)) {
                memset(dbuf, 0, ssize);
                ++R->bad;
                say(OFS_FATREC_WARN, "%s: sector %lu unreadable, saved as zeros", path, (unsigned long)(lba + j));
            }
            if (R->write(R->ctx, dbuf, len)) { failed = 1; break; }
            left -= len;
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
    ++R->files;
    if (guessed) ++R->guessed;
}

/* the entries of a directory; path is its path on the target */
static void walk_dir(struct dir *d, unsigned depth)
{
    ofs_u8 e[32];
    ofs_u32 pos, lba, sub;
    unsigned plen = (unsigned)strlen(path), nlfn = 0, n, i;
    ofs_u8 sum = 0;
    int haslfn = 0;
    for (pos = 0; pos < MAXENT; ++pos) {
        if (stopped()) return;
        if ((lba = entry_lba(d, pos)) == NOSEC) return;
        if (rsec(dbuf, &dsec, lba)) {
            ++R->bad;
            say(OFS_FATREC_WARN, "%s: a directory sector (%lu) is unreadable", plen ? path : "\\", (unsigned long)lba);
            pos |= (ssize / 32) - 1;     /* the rest of the sector */
            haslfn = 0;
            continue;
        }
        memcpy(e, dbuf + ((pos * 32) & (ssize - 1)), 32);
        if (e[0] == 0) return;
        if (e[0] == 0xE5) { haslfn = 0; continue; }
        if ((e[11] & 0x3F) == 0x0F) {    /* a long name part */
            unsigned seq = e[0] & 0x1F;
            static const ofs_u8 offs[13] = {1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30};
            if (e[0] & 0x40) { haslfn = 1; nlfn = seq; sum = e[13]; memset(lfn, 0, sizeof(lfn)); }
            if (!haslfn || !seq || seq > 20 || e[13] != sum) { haslfn = 0; continue; }
            for (i = 0; i < 13; ++i) lfn[(seq - 1) * 13 + i] = rd16(e + offs[i]);
            continue;
        }
        if (e[11] & 0x08) { haslfn = 0; continue; }     /* the volume label */
        if (e[0] == '.' && (e[1] == ' ' || (e[1] == '.' && e[2] == ' '))) { haslfn = 0; continue; }
        /* the name */
        n = plen;
        if (n + 2 >= sizeof(path)) return;
        path[n++] = '\\';
        if (haslfn && lfn_sum(e) == sum) {
            for (i = 0; i < nlfn * 13 && lfn[i] && lfn[i] != 0xFFFF; ++i) put_utf8(path, &n, sizeof(path), lfn[i]);
            path[n] = 0;
        } else {
            short_name(e, path + n, (unsigned)(sizeof(path) - n));
        }
        haslfn = 0;
        sub = rd16(e + 26) | (bits == 32 ? (ofs_u32)rd16(e + 20) << 16 : 0);
        if (e[11] & 0x10) {
            if (!valid(sub)) say(OFS_FATREC_WARN, "%s: a directory outside the volume, left out", path);
            else if (bit(GOT, sub)) say(OFS_FATREC_WARN, "%s: a directory seen before (a loop), left out", path);
            else if (depth >= MAXDEPTH) say(OFS_FATREC_WARN, "%s: too deep, left out", path);
            else walk(sub, depth + 1);
        } else {
            save_file(sub, rd32(e + 28), rd16(e + 22), rd16(e + 24));
        }
        path[plen] = 0;
        dsec = NOSEC;
    }
}

/* a directory at its first cluster, made on the target at path */
static void walk(ofs_u32 first, unsigned depth)
{
    struct dir d;
    ofs_u32 c, k;
    if (R->mkdir(R->ctx, path)) {
        ++R->failed;
        say(OFS_FATREC_ERROR, "%s: cannot be made on the target", path);
        return;
    }
    ++R->dirs;
    /* its clusters, so a loop through them is not walked again */
    for (c = first, k = 0; valid(c) && !bit(GOT, c) && k < MAXENT * 32 / (1UL << (sshift + cshift)) + 1; ++k) {
        set(GOT, c);
        c = next(c);
    }
    d.first = first, d.idx = 0, d.clu = first;
    walk_dir(&d, depth);
}

/* ---- the scan for lost directories ---- */

/* the first cluster of a directory: "." pointing at itself, ".." */
static int is_dir_start(ofs_u32 c, ofs_u32 *parent)
{
    const ofs_u8 *e = dbuf;
    if (rsec(dbuf, &dsec, clu_lba(c))) return 0;
    if (memcmp(e, ".          ", 11) || !(e[11] & 0x10) || memcmp(e + 32, "..         ", 11) || !(e[43] & 0x10))
        return 0;
    if ((rd16(e + 26) | (bits == 32 ? (ofs_u32)rd16(e + 20) << 16 : 0)) != c) return 0;
    *parent = rd16(e + 58) | (bits == 32 ? (ofs_u32)rd16(e + 52) << 16 : 0);
    return 1;
}

/* the candidate with first cluster c (they are sorted), or ncand */
static ofs_u32 find(ofs_u32 c)
{
    ofs_u32 lo = 0, hi = ncand, mid;
    while (lo < hi) {
        mid = lo + (hi - lo) / 2;
        if (cand(mid)[0] < c) lo = mid + 1; else hi = mid;
    }
    return lo < ncand && cand(lo)[0] == c ? lo : ncand;
}

static int pending(ofs_u32 c) { return find(c) != ncand && !bit(GOT, c); }

/* a lost directory whose parent is lost too and not saved yet waits for
   it: walking the parent saves it with its name.  Unless they make a loop:
   then this one starts. */
static int waits(ofs_u32 i)
{
    ofs_u32 p = cand(i)[1], k, j;
    if (!pending(p)) return 0;
    for (k = 0; k < MAXDEPTH; ++k) {
        if ((j = find(p)) == i) return 0;
        p = cand(j)[1];
        if (!pending(p)) return 1;
    }
    return 0;
}

static int scan(void)
{
    ofs_u32 c, p, i;
    int progress;
    say(OFS_FATREC_INFO, "Searching %lu clusters for lost directories", (unsigned long)nclu);
    for (c = 2; c < nclu + 2; ++c) {
        if (!(c & 1023) && stopped()) return OFS_FATREC_ESTOP;
        if (bit(GOT, c) || !is_dir_start(c, &p)) continue;
        if (2 * bmsize + (ncand + 1) * 8 > R->memsize) {
            say(OFS_FATREC_WARN, "Too many lost directories for the work memory; the rest left");
            break;
        }
        cand(ncand)[0] = c, cand(ncand)[1] = p;
        ++ncand;
    }
    do {
        progress = 0;
        for (i = 0; i < ncand; ++i) {
            if (bit(GOT, cand(i)[0]) || waits(i)) continue;
            if (!lost_no++ && R->mkdir(R->ctx, LOST)) ++R->failed;
            sprintf(path, LOST "\\DIR%05lu", (unsigned long)lost_no);
            say(OFS_FATREC_INFO, "A lost directory at cluster %lu saved as %s", (unsigned long)cand(i)[0], path);
            ++R->lost_dirs;
            walk(cand(i)[0], 1);
            path[0] = 0;
            progress = 1;
        }
    } while (progress);
    return 0;
}

/* ---- the chains nothing uses ---- */

static int chains(void)
{
    ofs_u32 c, v, csize = 1UL << (sshift + cshift);
    unsigned long no = 0;
    for (c = 2; c < nclu + 2; ++c)
        if (valid(v = next(c))) set(LINKED, v);
    for (c = 2; c < nclu + 2; ++c) {
        ofs_u32 n, x;
        if (!(c & 1023) && stopped()) return OFS_FATREC_ESTOP;
        if (bit(GOT, c) || bit(LINKED, c) || !links(next(c))) continue;
        /* its length */
        for (n = 1, x = c; n < nclu && valid(v = next(x)) && !bit(GOT, v) && v != c; ++n) x = v;
        if (!lost_no++ && R->mkdir(R->ctx, LOST)) ++R->failed;
        sprintf(path, LOST "\\FILE%05lu.CHK", ++no);
        ++R->lost_chains;
        say(OFS_FATREC_INFO, "A lost chain of %lu clusters at %lu saved as %s", (unsigned long)n, (unsigned long)c, path);
        save_file(c, n * csize, 0, 0);
        path[0] = 0;
    }
    return 0;
}

/* ---- the boot sector ---- */

static int is_pow2(ofs_u32 v) { return v && !(v & (v - 1)); }

static int bpb_ok(const ofs_u8 *b)
{
    ofs_u32 bps = rd16(b + 11);
    return bps >= 512 && bps <= OFS_FATREC_MAXSECT && is_pow2(bps) && is_pow2(b[13]) && rd16(b + 14) && b[16] &&
           (rd16(b + 19) || rd32(b + 32)) && (rd16(b + 22) || rd32(b + 36));
}

static int geometry(void)
{
    const ofs_u8 *b = dbuf;
    ofs_u32 total, data;
    if (R->read(R->dev, R->start, dbuf)) return OFS_FATREC_EIO;
    if (!bpb_ok(b)) {
        if (R->read(R->dev, R->start + 6, dbuf) || !bpb_ok(b) || rd16(b + 22)) return OFS_FATREC_EBAD;
        say(OFS_FATREC_WARN, "The boot sector is not FAT; using the backup in sector 6");
        R->from_backup = 1;
    }
    ssize = rd16(b + 11);
    for (sshift = 9; (1u << sshift) < ssize; ++sshift) ;
    for (cshift = 0; (1u << cshift) < b[13]; ++cshift) ;
    nfats = b[16];
    fat0 = rd16(b + 14);
    fatlen = rd16(b + 22) ? rd16(b + 22) : rd32(b + 36);
    rootsecs = ((ofs_u32)rd16(b + 17) * 32 + ssize - 1) >> sshift;
    rootsec = fat0 + nfats * fatlen;
    heap = rootsec + rootsecs;
    total = rd16(b + 19) ? rd16(b + 19) : rd32(b + 32);
    if (heap >= total) return OFS_FATREC_EBAD;
    data = total - heap;
    nclu = data >> cshift;
    active = 0;
    rootclu = 0;
    if (!rd16(b + 22)) {
        bits = 32;
        rootclu = rd32(b + 44);
        if (rd16(b + 40) & 0x80 && (rd16(b + 40) & 0x0F) < nfats) active = rd16(b + 40) & 0x0F;
        if (nclu > 0x0FFFFFF5UL) nclu = 0x0FFFFFF5UL;
    } else {
        bits = nclu < 4085 ? 12 : 16;
        if (nclu > 65524UL) nclu = 65524UL;
        if (!rootsecs) return OFS_FATREC_EBAD;
    }
    if ((((nclu + 2) * (bits / 4) + 1) / 2 + ssize - 1) >> sshift > fatlen) {
        say(OFS_FATREC_WARN, "The FAT is too small for %lu clusters; only those it holds", (unsigned long)nclu);
        nclu = (ofs_u32)(((fatlen << sshift) * 2) / (bits / 4)) - 2;
    }
    R->bits = bits, R->clusters = nclu, R->cluster_size = 1UL << (sshift + cshift);
    return 0;
}

int ofs_fatrec_run(struct ofs_fatrec *r)
{
    ofs_u32 off, len;
    int rc;
    R = r;
    r->bits = 0;
    r->files = r->dirs = r->bytes_lo = r->bytes_hi = r->guessed = r->bad = 0;
    r->lost_dirs = r->lost_chains = r->failed = 0;
    r->from_backup = 0;
    dbuf = r->buf, fbuf = r->buf + OFS_FATREC_MAXSECT;
    dsec = fsec = NOSEC;
    ncand = lost_no = 0;
    if ((rc = geometry()) != 0) return rc;
    dsec = NOSEC;
    bmsize = ((nclu + 2 + 7) / 8 + 7) & ~(ofs_u32)7;
    if (2 * bmsize > r->memsize) return OFS_FATREC_EMEM;
    for (off = 0; off < 2 * bmsize; off += len) {
        len = r->block - (off & (r->block - 1));
        if (len > 2 * bmsize - off) len = 2 * bmsize - off;
        FMEMSET(at(off), 0, (unsigned)len);
    }
    say(OFS_FATREC_INFO, "FAT%u, %lu clusters of %lu bytes, %u FAT%s", bits, (unsigned long)nclu,
        (unsigned long)r->cluster_size, nfats, nfats == 1 ? "" : "s");
    path[0] = 0;
    if (bits == 32) {
        if (!valid(rootclu)) say(OFS_FATREC_WARN, "The root directory starts outside the volume");
        else walk(rootclu, 0);
    } else {
        struct dir d;
        if (r->mkdir(r->ctx, "")) ++r->failed;
        ++r->dirs;
        d.first = 0, d.idx = 0, d.clu = 0;
        walk_dir(&d, 0);
    }
    if (stopped()) return OFS_FATREC_ESTOP;
    if (r->flags & OFS_FATREC_SCAN && (rc = scan()) != 0) return rc;
    if (r->flags & OFS_FATREC_CHAINS && (rc = chains()) != 0) return rc;
    return 0;
}
