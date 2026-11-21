/* SPDX-License-Identifier: GPL-2.0-or-later */
/* UDF reading: see include/ofs/udf.h. The layout as ECMA-167 (3rd
   edition) and the OSTA UDF specification 2.60 describe it. */
#include <string.h>
#include "ofs/udf.h"
#include "ofs/dostime.h"

#ifdef __WATCOMC__
#define fmemcpy _fmemcpy
#define fmemset _fmemset
#else
#define fmemcpy memcpy
#define fmemset memset
#endif

#define NOBLK 0xFFFFFFFFUL

/* descriptor tags */
#define T_PD   5
#define T_LVD  6
#define T_TD   8
#define T_AVDP 2
#define T_FSD  256
#define T_FID  257
#define T_AED  258
#define T_IE   259
#define T_FE   261
#define T_EFE  266

static ofs_u16 le16(const ofs_u8 *p) { return (ofs_u16)(p[0] | p[1] << 8); }
static ofs_u32 le32(const ofs_u8 *p)
{
    return (ofs_u32)p[0] | (ofs_u32)p[1] << 8 | (ofs_u32)p[2] << 16 | (ofs_u32)p[3] << 24;
}
static ofs_u64 le64(const ofs_u8 *p) { return (ofs_u64)le32(p) | (ofs_u64)le32(p + 4) << 32; }

#define BS(v) ((ofs_u32)1 << (v)->bshift)

/* a tag of that identifier with a good checksum */
static int tag(const ofs_u8 *p, unsigned id)
{
    unsigned i, sum = 0;
    for (i = 0; i < 16; ++i)
        if (i != 4) sum += p[i];
    return le16(p) == id && (ofs_u8)sum == p[4];
}

/* one logical block (absolute, in blocks of the volume) */
static int rdblk(struct ofs_udf *v, ofs_u32 blk, ofs_u8 OFS_FAR *dst)
{
    unsigned i, n = 1u << (v->bshift - v->sshift);
    ofs_lba s = v->start + ((ofs_lba)blk << (v->bshift - v->sshift));
    for (i = 0; i < n; ++i)
        if (v->read(v->dev, s + i, dst + ((ofs_u16)i << v->sshift))) return OFS_UDF_EIO;
    return 0;
}

static int cache(struct ofs_udf *v, ofs_u8 *buf, ofs_u32 *held, ofs_u32 blk)
{
    int r;
    if (*held == blk) return 0;
    *held = NOBLK;
    if ((r = rdblk(v, blk, buf))) return r;
    *held = blk;
    return 0;
}

static int locate(struct ofs_udf *v, struct ofs_udf_file *f, ofs_fsize off);

/* block lb of partition map part as a block of the volume */
static int resolve(struct ofs_udf *v, unsigned part, ofs_u32 lb, ofs_u32 *abs)
{
    const struct ofs_udf_map *m;
    int r;
    if (part >= v->nmaps) return OFS_UDF_EBAD;
    m = &v->map[part];
    if (!m->meta) {
        if (lb >= m->len) return OFS_UDF_EBAD;
        *abs = m->start + lb;
        return 0;
    }
    if ((r = locate(v, &v->meta, (ofs_fsize)lb << v->bshift))) return r;
    if (v->meta.ext_zero) return OFS_UDF_EBAD;
    *abs = v->meta.ext_abs + (ofs_u32)((((ofs_fsize)lb << v->bshift) - v->meta.ext_off) >> v->bshift);
    return 0;
}

/* the file entry of f into its buffer (the metadata file's own); NULL on error */
static const ofs_u8 *entry(struct ofs_udf *v, struct ofs_udf_file *f, int *err)
{
    ofs_u32 abs;
    int is_meta = f == &v->meta;
    ofs_u8 *b = is_meta ? v->mfe : v->fe;
    if ((*err = resolve(v, f->icb_part, f->icb_lb, &abs))) return NULL;
    if ((*err = cache(v, b, is_meta ? &v->mfe_blk : &v->fe_blk, abs))) return NULL;
    if (tag(b, T_IE)) { *err = OFS_UDF_EUNSUP; return NULL; }
    if (!tag(b, T_FE) && !tag(b, T_EFE)) { *err = OFS_UDF_EBAD; return NULL; }
    return b;
}

/* where the allocation descriptors (or the data inside) of an entry are */
static int ads(const struct ofs_udf *v, const ofs_u8 *e, unsigned *at, unsigned *len)
{
    int efe = le16(e) == T_EFE;
    ofs_u32 lea = le32(e + (efe ? 208 : 168)), lad = le32(e + (efe ? 212 : 172));
    ofs_u32 hdr = efe ? 216 : 176;
    if (lea > BS(v) || lad > BS(v) || hdr + lea + lad > BS(v)) return OFS_UDF_EBAD;
    *at = (unsigned)(hdr + lea);
    *len = (unsigned)lad;
    return 0;
}

/* fills f from its entry (in the entry buffer) */
static int stat_of(struct ofs_udf *v, struct ofs_udf_file *f, ofs_u16 *time, ofs_u16 *date)
{
    const ofs_u8 *e;
    unsigned type, at, len;
    int r;
    f->ext_len = 0;
    f->ext_off = 0;
    if (!(e = entry(v, f, &r))) {
        if (r != OFS_UDF_EUNSUP) return r;
        f->unsup = 1;
        f->size = 0;
        return 0;
    }
    if ((r = ads(v, e, &at, &len))) return r;
    type = e[16 + 11];
    f->dir = type == 4;
    f->inl = (le16(e + 16 + 18) & 7) == 3;
    {
        ofs_u64 size = le64(e + 56);
        if (f->inl && size > len) return OFS_UDF_EBAD;
#ifdef OFS_NARROW
        if (size > 0xFFFFFFFFUL) size = 0xFFFFFFFFUL;    /* cut, as NTFS's "big" */
#endif
        f->size = (ofs_fsize)size;
    }
    if (time) {
        const ofs_u8 *t = e + (le16(e) == T_EFE ? 92 : 84);
        ofs_s32 zone = le16(t) & 0xFFF;
        if (zone & 0x800) zone -= 0x1000;
        if (zone == -2047 || (le16(t) >> 12) != 1) zone = v->tz;   /* none given: local */
        ofs_dostime((ofs_s32)le16(t + 2), t[4], t[5], t[6], t[7], t[8], zone, v->tz, time, date);
    }
    return 0;
}

/* the extent of f that holds off into its cache (f->ext_*) */
static int locate(struct ofs_udf *v, struct ofs_udf_file *f, ofs_fsize off)
{
    const ofs_u8 *e, *p;
    unsigned at, len, adtype, sz, i, hops = 0;
    ofs_fsize acc = 0;
    int r;
    if (f->ext_len && off >= f->ext_off && off - f->ext_off < f->ext_len) return 0;
    if (!(e = entry(v, f, &r))) return r;
    if ((r = ads(v, e, &at, &len))) return r;
    adtype = le16(e + 16 + 18) & 7;
    sz = adtype == 0 ? 8 : adtype == 1 ? 16 : adtype == 2 ? 20 : 0;
    if (!sz) return OFS_UDF_EBAD;
    p = e + at;
    for (i = 0;; i += sz) {
        ofs_u32 l, lb, elen;
        unsigned t, part;
        if (i + sz > len) return OFS_UDF_EBAD;       /* off past the extents */
        l = le32(p + i);
        elen = l & 0x3FFFFFFFUL;
        t = (unsigned)(l >> 30);
        if (adtype == 0) { lb = le32(p + i + 4); part = f->icb_part; }
        else if (adtype == 1) { lb = le32(p + i + 4); part = le16(p + i + 8); }
        else { lb = le32(p + i + 12); part = le16(p + i + 16); }
        if (!elen) return OFS_UDF_EBAD;
        if (t == 3) {             /* the descriptors go on in an allocation extent */
            ofs_u32 abs, n;
            if (++hops > 1024) return OFS_UDF_EBAD;
            if ((r = resolve(v, part, lb, &abs))) return r;
            if ((r = cache(v, v->aed, &v->aed_blk, abs))) return r;
            if (!tag(v->aed, T_AED)) return OFS_UDF_EBAD;
            n = le32(v->aed + 20);
            if (n > BS(v) - 24) return OFS_UDF_EBAD;
            p = v->aed + 24;
            len = (unsigned)n;
            i = (unsigned)-sz;
            continue;
        }
        if (off < acc + elen) {
            f->ext_off = acc;
            f->ext_len = elen;
            f->ext_zero = t != 0;
            f->ext_abs = 0;
            if (!f->ext_zero) {
                ofs_u32 abs;
                if ((r = resolve(v, part, lb, &abs))) { f->ext_len = 0; return r; }
                f->ext_abs = abs;
            }
            return 0;
        }
        acc += elen;
    }
}

/* bytes of a file into a near buffer (directories) */
static int bytes(struct ofs_udf *v, struct ofs_udf_file *f, ofs_fsize off, ofs_u8 *dst, unsigned n)
{
    int r;
    if (off > f->size || n > f->size - off) return OFS_UDF_EBAD;
    if (f->inl) {
        const ofs_u8 *e;
        unsigned at, len;
        if (!(e = entry(v, f, &r))) return r;
        if ((r = ads(v, e, &at, &len))) return r;
        memcpy(dst, e + at + (unsigned)off, n);
        return 0;
    }
    while (n) {
        ofs_u32 in, o;
        unsigned k;
        if ((r = locate(v, f, off))) return r;
        in = (ofs_u32)(off - f->ext_off);
        o = in & (BS(v) - 1);
        k = (unsigned)(BS(v) - o);
        if (k > n) k = n;
        if (k > f->ext_len - in) k = (unsigned)(f->ext_len - in);
        if (f->ext_zero) memset(dst, 0, k);
        else {
            if ((r = cache(v, v->blk, &v->blk_blk, f->ext_abs + (in >> v->bshift)))) return r;
            memcpy(dst, v->blk + (unsigned)o, k);
        }
        dst += k;
        off += k;
        n -= k;
    }
    return 0;
}

/* ---- names ---- */

/* OSTA compressed Unicode (d-characters): 8 bits per character, or 16 big-endian */
static unsigned dchars(const ofs_u8 *p, unsigned n, ofs_u16 *out, unsigned max)
{
    unsigned i, k = 0;
    if (!n) return 0;
    if (p[0] == 8 || p[0] == 254)
        for (i = 1; i < n && k < max; ++i) out[k++] = p[i];
    else if (p[0] == 16 || p[0] == 255)
        for (i = 1; i + 1 < n && k < max; i += 2) out[k++] = (ofs_u16)(p[i] << 8 | p[i + 1]);
    return k;
}

/* ---- directories ---- */

int ofs_udf_dir_next(struct ofs_udf *v, struct ofs_udf_file *dir, ofs_u32 *pos, struct ofs_udf_ent *e)
{
    static ofs_u8 fid[38 + 256];
    for (;;) {
        ofs_u32 at = *pos, total;
        unsigned lfi, liu, chars;
        int r;
        if ((ofs_fsize)*pos + 38 > dir->size) return OFS_UDF_END;
        if ((r = bytes(v, dir, *pos, fid, 38))) return r;
        if (!tag(fid, T_FID)) {
            /* padding to the end of a block (some writers) */
            if (!le16(fid) && !fid[4]) {
                ofs_u32 next = (*pos | (BS(v) - 1)) + 1;
                if (next <= *pos) return OFS_UDF_END;
                *pos = next;
                continue;
            }
            return OFS_UDF_EBAD;
        }
        chars = fid[18];
        lfi = fid[19];
        liu = le16(fid + 36);
        total = ((ofs_u32)38 + liu + lfi + 3) & ~(ofs_u32)3;
        if ((ofs_fsize)*pos + total > dir->size) return OFS_UDF_EBAD;
        memset(e, 0, sizeof(*e));
        e->pos = at;
        e->file.icb_lb = le32(fid + 24);
        e->file.icb_part = le16(fid + 28);
        *pos += total;
        if (chars & (4 | 8)) continue;          /* deleted, the parent */
        if ((r = bytes(v, dir, (ofs_fsize)at + 38 + liu, fid + 38, lfi))) return r;
        e->name_len = (ofs_u8)dchars(fid + 38, lfi, e->name, 255);
        if (!e->name_len) continue;
        if ((r = stat_of(v, &e->file, &e->time, &e->date))) return r;
        if (chars & 2) e->file.dir = 1;
        e->attr = OFS_UDF_RDONLY | (e->file.dir ? OFS_UDF_DIR : 0) | ((chars & 1) ? OFS_UDF_HIDDEN : 0);
        return 0;
    }
}

void ofs_udf_rootdir(const struct ofs_udf *v, struct ofs_udf_file *f) { *f = v->root; }

/* ---- reading ---- */

int ofs_udf_read(struct ofs_udf *v, struct ofs_udf_file *f, ofs_fsize off, void OFS_FAR *buf, ofs_u16 len,
                 ofs_u16 *done)
{
    ofs_u8 OFS_FAR *p = (ofs_u8 OFS_FAR *)buf;
    int r;
    *done = 0;
    if (f->unsup) return OFS_UDF_EUNSUP;
    if (off >= f->size) return 0;
    if (f->size - off < len) len = (ofs_u16)(f->size - off);
    if (f->inl) {
        const ofs_u8 *e;
        unsigned at, alen;
        if (!(e = entry(v, f, &r))) return r;
        if ((r = ads(v, e, &at, &alen))) return r;
        fmemcpy(p, e + at + (unsigned)off, len);
        *done = len;
        return 0;
    }
    while (len) {
        ofs_u32 in, o, blk;
        unsigned n;
        if ((r = locate(v, f, off))) return r;
        in = (ofs_u32)(off - f->ext_off);
        o = in & (BS(v) - 1);
        blk = f->ext_abs + (in >> v->bshift);
        n = (unsigned)(BS(v) - o);
        if (n > len) n = len;
        if (n > f->ext_len - in) n = (unsigned)(f->ext_len - in);
        if (f->ext_zero) fmemset(p, 0, n);
        else if (!o && n == BS(v)) {
            if ((r = rdblk(v, blk, p))) return r;   /* straight into the caller's buffer */
        } else {
            if ((r = cache(v, v->blk, &v->blk_blk, blk))) return r;
            fmemcpy(p, v->blk + (unsigned)o, n);
        }
        p += n;
        off += n;
        len = (ofs_u16)(len - n);
        *done = (ofs_u16)(*done + n);
    }
    return 0;
}

/* ---- mount ---- */

struct pd { ofs_u16 num; ofs_u32 start, len; };

int ofs_udf_mount(struct ofs_udf *v, ofs_udf_read_fn read, void *dev, ofs_lba start, unsigned sshift,
                  const struct ofs_cp *cp, ofs_u8 *buf)
{
    struct pd pds[OFS_UDF_MAPS];
    unsigned npd = 0, i, b, lvd = 0;
    ofs_u32 vds, vdslen, fsd_lb = 0, meta_loc = 0;
    ofs_u16 fsd_part = 0;
    ofs_u8 *d;
    int r;

    memset(v, 0, sizeof(*v));
    if (sshift < 9 || sshift > 12) return OFS_UDF_EBAD;
    v->read = read; v->dev = dev; v->start = start; v->sshift = (ofs_u8)sshift; v->cp = cp;
    v->fe = buf; v->mfe = buf + OFS_UDF_MAXBLOCK; v->aed = buf + 2 * OFS_UDF_MAXBLOCK;
    v->blk = buf + 3 * OFS_UDF_MAXBLOCK;
    v->fe_blk = v->mfe_blk = v->aed_blk = v->blk_blk = NOBLK;
    d = v->blk;
    /* the anchor at block 256, for each block size the device allows:
       2048 first (optical media, mkudffs), then 512, 1024, 4096 */
    {
        static const ofs_u8 try_[4] = { 11, 9, 10, 12 };
        for (b = 0; b < 4; ++b) {
            if (try_[b] < sshift) continue;
            v->bshift = try_[b];
            if ((r = rdblk(v, 256, d))) continue;   /* a small device */
            if (tag(d, T_AVDP) && le32(d + 12) == 256) break;
        }
        if (b == 4) return OFS_UDF_EBAD;
    }
    vdslen = le32(d + 16);
    vds = le32(d + 20);
    for (i = 0; i < 64 && (ofs_u32)i << v->bshift < vdslen; ++i) {
        if ((r = rdblk(v, vds + i, d))) return r;
        if (tag(d, T_TD)) break;
        if (tag(d, T_PD) && npd < OFS_UDF_MAPS) {
            pds[npd].num = le16(d + 22);
            pds[npd].start = le32(d + 188);
            pds[npd].len = le32(d + 192);
            ++npd;
        } else if (tag(d, T_LVD) && !lvd) {
            ofs_u32 nmaps = le32(d + 268), mtl = le32(d + 264), o = 0;
            unsigned k, l = d[84 + 127];
            if (le32(d + 212) != BS(v)) return OFS_UDF_ESIZE;
            if (l > 127) l = 127;
            v->label_len = (ofs_u8)dchars(d + 84, l, v->label, 63);
            fsd_lb = le32(d + 248 + 4);
            fsd_part = le16(d + 248 + 8);
            if (440 + mtl > BS(v)) return OFS_UDF_EBAD;
            for (k = 0; k < nmaps && k < OFS_UDF_MAPS; ++k) {
                const ofs_u8 *m = d + 440 + o;
                unsigned ml;
                if (o + 2 > mtl || (ml = m[1]) < 6 || o + ml > mtl) return OFS_UDF_EBAD;
                if (m[0] == 1) v->map[k].pnum = le16(m + 4);
                else if (m[0] == 2 && ml >= 64) {
                    const char *id = (const char *)m + 5;
                    v->map[k].pnum = le16(m + 38);
                    if (!memcmp(id, "*UDF Metadata Partition", 23)) {
                        v->map[k].meta = 1;
                        meta_loc = le32(m + 40);
                    } else if (!memcmp(id, "*UDF Virtual Partition", 22)) return OFS_UDF_EUNSUP;
                    else if (memcmp(id, "*UDF Sparable Partition", 23)) return OFS_UDF_EUNSUP;
                } else return OFS_UDF_EUNSUP;
                o += ml;
            }
            v->nmaps = (ofs_u8)k;
            lvd = 1;
        }
    }
    if (!lvd || !npd || !v->nmaps) return OFS_UDF_EBAD;
    /* the partitions' places */
    for (i = 0; i < v->nmaps; ++i) {
        unsigned k;
        for (k = 0; k < npd && pds[k].num != v->map[i].pnum; ++k)
            ;
        if (k == npd) return OFS_UDF_EBAD;
        v->map[i].start = pds[k].start;
        v->map[i].len = pds[k].len;
        if (!v->blocks) v->blocks = pds[k].len;
    }
    /* a metadata partition: its file is in the physical partition of the same number */
    for (i = 0; i < v->nmaps; ++i)
        if (v->map[i].meta) {
            unsigned k;
            for (k = 0; k < v->nmaps && (v->map[k].meta || v->map[k].pnum != v->map[i].pnum); ++k)
                ;
            if (k == v->nmaps) return OFS_UDF_EBAD;
            v->meta.icb_part = (ofs_u16)k;
            v->meta.icb_lb = meta_loc;
            if ((r = stat_of(v, &v->meta, NULL, NULL))) return r;
            if (v->meta.inl || v->meta.unsup) return OFS_UDF_EUNSUP;
            break;
        }
    /* the file set descriptor: the root directory */
    {
        ofs_u32 abs;
        if ((r = resolve(v, fsd_part, fsd_lb, &abs))) return r;
        if ((r = cache(v, v->blk, &v->blk_blk, abs))) return r;
        if (!tag(v->blk, T_FSD)) return OFS_UDF_EBAD;
        v->root.icb_lb = le32(v->blk + 400 + 4);
        v->root.icb_part = le16(v->blk + 400 + 8);
        if ((r = stat_of(v, &v->root, NULL, NULL))) return r;
        if (v->root.unsup) return OFS_UDF_EUNSUP;
        v->root.dir = 1;
    }
    while (v->label_len && v->label[v->label_len - 1] == ' ') --v->label_len;
    return 0;
}
