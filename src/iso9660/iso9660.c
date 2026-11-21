/* SPDX-License-Identifier: GPL-2.0-or-later */
/* ISO 9660 reading: see include/ofs/iso9660.h. The layout as ECMA-119
   (2nd edition) describes it, Joliet as Microsoft's specification, the
   System Use Sharing Protocol and Rock Ridge as IEEE P1281/P1282. */
#include <string.h>
#include "ofs/iso9660.h"
#include "ofs/dostime.h"

#ifdef __WATCOMC__
#define fmemcpy _fmemcpy
#else
#define fmemcpy memcpy
#endif

#define NOBLK 0xFFFFFFFFUL

static ofs_u16 le16(const ofs_u8 *p) { return (ofs_u16)(p[0] | p[1] << 8); }
static ofs_u32 le32(const ofs_u8 *p)
{
    return (ofs_u32)p[0] | (ofs_u32)p[1] << 8 | (ofs_u32)p[2] << 16 | (ofs_u32)p[3] << 24;
}

/* one logical block into dst (2048 bytes) */
static int rdblk(struct ofs_iso *v, ofs_u32 blk, ofs_u8 OFS_FAR *dst)
{
    unsigned i, n = 1u << (11 - v->sshift);
    ofs_lba s = v->start + ((ofs_lba)blk << (11 - v->sshift));
    for (i = 0; i < n; ++i)
        if (v->read(v->dev, s + i, dst + ((ofs_u16)i << v->sshift))) return OFS_ISO_EIO;
    return 0;
}

static int block(struct ofs_iso *v, ofs_u32 blk)
{
    int r;
    if (v->buf_blk == blk) return 0;
    v->buf_blk = NOBLK;
    if ((r = rdblk(v, blk, v->buf))) return r;
    v->buf_blk = blk;
    return 0;
}

static int ceblock(struct ofs_iso *v, ofs_u32 blk)
{
    int r;
    if (v->ce_blk == blk) return 0;
    v->ce_blk = NOBLK;
    if ((r = rdblk(v, blk, v->ce))) return r;
    v->ce_blk = blk;
    return 0;
}

/* ---- times ---- */

/* the 7-byte time of a directory record (local with its offset from UTC
   in 15 minutes) as a DOS time of the host's zone */
static void dostime(const struct ofs_iso *v, const ofs_u8 *t, ofs_u16 *time, ofs_u16 *date)
{
    signed char off = (signed char)t[6];
    if (off < -48 || off > 52) off = 0;
    ofs_dostime(1900 + (ofs_s32)t[0], t[1], t[2], t[3], t[4], t[5], (ofs_s32)off * 15, v->tz, time, date);
}

/* ---- names ---- */

/* UTF-8 (Rock Ridge has bytes; today's writers put UTF-8) appended to
   ent->name; a byte that is no UTF-8 is taken as Latin-1 */
static void put8(struct ofs_iso_ent *e, const ofs_u8 *p, unsigned n)
{
    unsigned i = 0;
    while (i < n && e->name_len < 255) {
        ofs_u32 c = p[i];
        unsigned k = 0;
        if (c >= 0xC2 && c < 0xE0) k = 1;
        else if (c >= 0xE0 && c < 0xF0) k = 2;
        else if (c >= 0xF0 && c < 0xF5) k = 3;
        if (k && i + k < n) {
            unsigned j;
            ofs_u32 u = c & (0x3F >> k);
            for (j = 1; j <= k; ++j) {
                if (i + j >= n || (p[i + j] & 0xC0) != 0x80) break;
                u = u << 6 | (p[i + j] & 0x3F);
            }
            if (j > k) {
                i += k + 1;
                if (u >= 0x10000UL) {
                    if (e->name_len + 2 > 255) return;
                    u -= 0x10000UL;
                    e->name[e->name_len++] = (ofs_u16)(0xD800 | u >> 10);
                    e->name[e->name_len++] = (ofs_u16)(0xDC00 | (u & 0x3FF));
                } else e->name[e->name_len++] = (ofs_u16)u;
                continue;
            }
        }
        e->name[e->name_len++] = (ofs_u16)c;
        ++i;
    }
}

/* the plain ISO name: without ";version" and a trailing "." */
static void isoname(const struct ofs_iso *v, struct ofs_iso_ent *e, const ofs_u8 *p, unsigned n)
{
    unsigned i;
    if (v->joliet) {
        n &= ~1u;
        for (i = 0; i + 1 < n && e->name_len < 255; i += 2) e->name[e->name_len++] = (ofs_u16)(p[i] << 8 | p[i + 1]);
    } else
        for (i = 0; i < n && e->name_len < 255; ++i) {
            ofs_u8 c = p[i];
            e->name[e->name_len++] = c < 0x80 || !v->cp ? c : v->cp->uni[c - 0x80];
        }
    for (i = 0; i < e->name_len; ++i)
        if (e->name[i] == ';') { e->name_len = (ofs_u8)i; break; }
    if (e->name_len > 1 && e->name[e->name_len - 1] == '.') --e->name_len;
}

/* ---- System Use (Rock Ridge) ---- */

struct susp {
    ofs_u8 nm;                   /* an NM name was seen */
    ofs_u8 nm_more;              /* its CONTINUE flag */
    ofs_u8 re;                   /* relocated (RE): left out */
    ofs_u32 cl;                  /* child link (CL): the directory, 0 none */
};

/* the entries of one system use area (or continuation area); *ce_blk etc.
   the continuation the area names (CE), 0 none */
static void area(struct ofs_iso_ent *e, struct susp *s, const ofs_u8 *p, unsigned n,
                 ofs_u32 *ce_blk, ofs_u32 *ce_off, ofs_u32 *ce_len)
{
    unsigned i = 0;
    while (i + 4 <= n) {
        const ofs_u8 *q = p + i;
        unsigned l = q[2];
        if (l < 4 || i + l > n) break;
        if (q[0] == 'S' && q[1] == 'T') break;
        if (q[0] == 'C' && q[1] == 'E' && l >= 28) {
            *ce_blk = le32(q + 4); *ce_off = le32(q + 12); *ce_len = le32(q + 20);
        } else if (q[0] == 'N' && q[1] == 'M' && l >= 5) {
            if (!s->nm) e->name_len = 0;
            if (!(q[4] & 6)) {        /* not "." or ".." */
                if (!s->nm || s->nm_more) put8(e, q + 5, l - 5);
                s->nm = 1;
                s->nm_more = (q[4] & 1) != 0;
            }
        } else if (q[0] == 'R' && q[1] == 'E') s->re = 1;
        else if (q[0] == 'C' && q[1] == 'L' && l >= 12) s->cl = le32(q + 4);
        i += l;
    }
}

/* Rock Ridge for the record r (in v->buf, which stays valid) */
static int rrip(struct ofs_iso *v, struct ofs_iso_ent *e, struct susp *s, const ofs_u8 *r)
{
    unsigned rl = r[0], nl = r[32];
    unsigned su = 33 + nl + !(nl & 1) + v->skip;
    ofs_u32 cb = 0, co = 0, cn = 0;
    unsigned hops = 0;
    if (su < rl) area(e, s, r + su, rl - su, &cb, &co, &cn);
    while (cb && hops++ < 8) {
        ofs_u32 b = cb, o = co, n = cn;
        int x;
        cb = 0;
        if (o >= OFS_ISO_BLOCK || n > OFS_ISO_BLOCK - o || b >= v->blocks) return OFS_ISO_EBAD;
        if ((x = ceblock(v, b))) return x;
        area(e, s, v->ce + (unsigned)o, (unsigned)n, &cb, &co, &cn);
    }
    return 0;
}

/* ---- directories ---- */

/* the record at *pos of a directory, *pos after it; NULL with *err at the end */
static const ofs_u8 *record(struct ofs_iso *v, ofs_u32 lba, ofs_u32 size, ofs_u32 *pos, int *err)
{
    for (;;) {
        unsigned off, rl;
        const ofs_u8 *r;
        *err = OFS_ISO_END;
        if (*pos >= size) return NULL;
        off = (unsigned)(*pos & (OFS_ISO_BLOCK - 1));
        if ((*err = block(v, lba + (*pos >> 11)))) return NULL;
        r = v->buf + off;
        rl = r[0];
        if (!rl) {                /* no record crosses a block: the rest is padding */
            *pos = (*pos | (OFS_ISO_BLOCK - 1)) + 1;
            continue;
        }
        if (rl < 34 || off + rl > OFS_ISO_BLOCK || 33u + r[32] > rl) { *err = OFS_ISO_EBAD; return NULL; }
        *pos += rl;
        *err = 0;
        return r;
    }
}

static void file_of(struct ofs_iso_file *f, const ofs_u8 *r)
{
    memset(f, 0, sizeof(*f));
    f->lba = le32(r + 2) + r[1];  /* after its extended attribute record */
    f->len = le32(r + 10);
    f->size = f->len;
    f->dir = (r[25] & 2) != 0;
    f->unsup = r[26] || r[27];
    f->ext_lba = f->lba;
    f->ext_len = f->len;
}

int ofs_iso_dir_next(struct ofs_iso *v, struct ofs_iso_file *dir, ofs_u32 *pos, struct ofs_iso_ent *e)
{
    for (;;) {
        struct susp s;
        const ofs_u8 *r;
        ofs_u32 at;
        int err;
        r = record(v, dir->lba, dir->size, pos, &err);
        if (!r) return err;
        at = *pos - r[0];
        if (r[32] == 1 && (r[33] == 0 || r[33] == 1)) continue;   /* "." and ".." */
        memset(e, 0, sizeof(*e));
        e->pos = at;
        file_of(&e->file, r);
        e->file.dir_lba = dir->lba;
        e->file.dir_size = dir->size;
        e->file.rec = at;
        e->attr = OFS_ISO_RDONLY | (e->file.dir ? OFS_ISO_DIR : 0) | ((r[25] & 1) ? OFS_ISO_HIDDEN : 0);
        dostime(v, r + 18, &e->time, &e->date);
        memset(&s, 0, sizeof(s));
        if (v->rr && (err = rrip(v, e, &s, r))) return err;
        /* rrip may have read a continuation block, never v->buf: r stays */
        if (!s.nm) isoname(v, e, r + 33, r[32]);
        if (s.re) continue;
        if (r[25] & 0x80) {       /* further extents follow, the last without the flag */
            e->file.multi = 1;
            for (;;) {
                const ofs_u8 *x = record(v, dir->lba, dir->size, pos, &err);
                if (!x) return err == OFS_ISO_END ? OFS_ISO_EBAD : err;
                e->file.size += le32(x + 10);
                if (!(x[25] & 0x80)) break;
            }
        }
        if (s.cl) {               /* a deep directory moved elsewhere: its "." gives the size */
            ofs_u32 p = 0;
            const ofs_u8 *d;
            if (s.cl >= v->blocks) return OFS_ISO_EBAD;
            d = record(v, s.cl, OFS_ISO_BLOCK, &p, &err);
            if (!d) return err == OFS_ISO_END ? OFS_ISO_EBAD : err;
            e->file.lba = e->file.ext_lba = s.cl;
            e->file.len = e->file.ext_len = le32(d + 10);
            e->file.size = e->file.len;
            e->file.dir = 1;
            e->attr |= OFS_ISO_DIR;
        }
        if (!e->name_len) continue;
        return 0;
    }
}

void ofs_iso_rootdir(const struct ofs_iso *v, struct ofs_iso_file *f)
{
    memset(f, 0, sizeof(*f));
    f->lba = f->ext_lba = v->root_lba;
    f->len = f->ext_len = v->root_size;
    f->size = v->root_size;
    f->dir = 1;
}

/* ---- reading ---- */

/* the extent of a file that holds off into ext_* */
static int extent(struct ofs_iso *v, struct ofs_iso_file *f, ofs_fsize off)
{
    ofs_u32 pos = f->rec;
    ofs_fsize at = 0;
    int err;
    if (off >= f->ext_off && off - f->ext_off < f->ext_len) return 0;
    if (!f->multi) return OFS_ISO_EBAD;
    for (;;) {
        const ofs_u8 *r = record(v, f->dir_lba, f->dir_size, &pos, &err);
        ofs_u32 len;
        if (!r) return err == OFS_ISO_END ? OFS_ISO_EBAD : err;
        len = le32(r + 10);
        if (off - at < len) {
            f->ext_off = at;
            f->ext_lba = le32(r + 2) + r[1];
            f->ext_len = len;
            return 0;
        }
        at += len;
        if (!(r[25] & 0x80)) return OFS_ISO_EBAD;
    }
}

int ofs_iso_read(struct ofs_iso *v, struct ofs_iso_file *f, ofs_fsize off, void OFS_FAR *buf, ofs_u16 len,
                 ofs_u16 *done)
{
    ofs_u8 OFS_FAR *p = (ofs_u8 OFS_FAR *)buf;
    int r;
    *done = 0;
    if (f->unsup) return OFS_ISO_EUNSUP;
    while (len && off < f->size) {
        ofs_u32 in, blk;
        unsigned o, n;
        if ((r = extent(v, f, off))) return r;
        in = (ofs_u32)(off - f->ext_off);
        blk = f->ext_lba + (in >> 11);
        o = (unsigned)(in & (OFS_ISO_BLOCK - 1));
        n = OFS_ISO_BLOCK - o;
        if (n > len) n = len;
        if (n > f->ext_len - in) n = (unsigned)(f->ext_len - in);
        if (f->size - off < n) n = (unsigned)(f->size - off);
        if (blk >= v->blocks) return OFS_ISO_EBAD;
        if (!o && n == OFS_ISO_BLOCK) {
            if ((r = rdblk(v, blk, p))) return r;   /* straight into the caller's buffer */
        } else {
            if ((r = block(v, blk))) return r;
            fmemcpy(p, v->buf + o, n);
        }
        p += n;
        off += n;
        len = (ofs_u16)(len - n);
        *done = (ofs_u16)(*done + n);
    }
    return 0;
}

/* ---- mount ---- */

int ofs_iso_mount(struct ofs_iso *v, ofs_iso_read_fn read, void *dev, ofs_lba start, unsigned sshift,
                  const struct ofs_cp *cp, ofs_u8 *buf)
{
    ofs_u32 blk, proot = 0, psize = 0, jroot = 0, jsize = 0;
    ofs_u8 plabel[32], jlabel[32];
    int r, pvd = 0, svd = 0;
    unsigned i;

    memset(v, 0, sizeof(*v));
    if (sshift < 9 || sshift > 11) return OFS_ISO_EBAD;  /* none on such a device */
    v->read = read; v->dev = dev; v->start = start; v->sshift = (ofs_u8)sshift; v->cp = cp;
    v->buf = buf; v->ce = buf + OFS_ISO_BLOCK;
    v->buf_blk = v->ce_blk = NOBLK;
    v->blocks = 0xFFFFFFFFUL;
    for (blk = 16; blk < 16 + 64; ++blk) {
        const ofs_u8 *d = v->buf;
        if ((r = block(v, blk))) return r;
        if (memcmp(d + 1, "CD001", 5) || d[6] != 1) {
            if (!pvd) return OFS_ISO_EBAD;
            break;                /* the set ends without its terminator */
        }
        if (d[0] == 255) break;
        if (d[0] == 1 && !pvd) {
            if (le16(d + 128) != OFS_ISO_BLOCK) return OFS_ISO_ESIZE;
            v->blocks = le32(d + 80);
            proot = le32(d + 156 + 2) + d[156 + 1];
            psize = le32(d + 156 + 10);
            memcpy(plabel, d + 40, 32);
            pvd = 1;
        } else if (d[0] == 2 && !svd && d[88] == '%' && d[89] == '/' &&
                   (d[90] == '@' || d[90] == 'C' || d[90] == 'E')) {
            jroot = le32(d + 156 + 2) + d[156 + 1];
            jsize = le32(d + 156 + 10);
            memcpy(jlabel, d + 40, 32);
            svd = 1;
        }
    }
    if (!pvd || !psize || proot >= v->blocks) return OFS_ISO_EBAD;
    /* SUSP: the first entry of the root's "." record is SP (BEh EFh) */
    {
        ofs_u32 pos = 0;
        const ofs_u8 *d = record(v, proot, psize, &pos, &r);
        if (!d) return r == OFS_ISO_END ? OFS_ISO_EBAD : r;
        i = 33 + d[32] + !(d[32] & 1);
        if (i + 7 <= d[0] && d[i] == 'S' && d[i + 1] == 'P' && d[i + 4] == 0xBE && d[i + 5] == 0xEF) {
            v->rr = 1;
            v->skip = d[i + 6];
        }
    }
    if (v->rr || !svd || jroot >= v->blocks) {
        v->root_lba = proot; v->root_size = psize;
        for (i = 0; i < 32; ++i) v->label[i] = plabel[i];
        v->label_len = 32;
    } else {
        v->joliet = 1;
        v->root_lba = jroot; v->root_size = jsize;
        for (i = 0; i < 16; ++i) v->label[i] = (ofs_u16)(jlabel[2 * i] << 8 | jlabel[2 * i + 1]);
        v->label_len = 16;
    }
    while (v->label_len && (v->label[v->label_len - 1] == ' ' || !v->label[v->label_len - 1])) --v->label_len;
    return 0;
}
