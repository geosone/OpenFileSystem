/* SPDX-License-Identifier: GPL-2.0-or-later */
/* NTFS reading: see include/openfat/ntfs.h.  The structures and offsets
   follow ntfs-3g's include/ntfs-3g/layout.h (GPL-2.0-or-later). */
#include <string.h>
#include "openfat/ntfs.h"

#ifdef __WATCOMC__
#include <string.h>
#define fmemcpy _fmemcpy
#define fmemset _fmemset
#else
#define fmemcpy memcpy
#define fmemset memset
#endif

#define NONE 0xFFFFFFFFUL
#define NOBUF ((ofat_lba)~(ofat_lba)0)

/* attribute types */
#define AT_STD      0x10
#define AT_LIST     0x20
#define AT_FNAME    0x30
#define AT_VNAME    0x60
#define AT_VINFO    0x70
#define AT_DATA     0x80
#define AT_IROOT    0x90
#define AT_IALLOC   0xA0
#define AT_BITMAP   0xB0
#define AT_END      0xFFFFFFFFUL

#define F_DIR       0x10000000UL        /* $FILE_NAME flags: a directory */

static ofat_u16 rd16(const ofat_u8 *p) { return (ofat_u16)(p[0] | p[1] << 8); }
static ofat_u32 rd32(const ofat_u8 *p) { return rd16(p) | (ofat_u32)rd16(p + 2) << 16; }

static const ofat_u8 *sector(struct ofat_ntfs *v, ofat_lba lba)
{
    if (v->buf_lba != lba) {
        v->buf_lba = NOBUF;
        if (v->read(v->dev, v->start + lba, v->buf)) return 0;
        v->buf_lba = lba;
    }
    return v->buf;
}

/* ---- runlists ---- */

/* The run of VCN vcn in the runlist at rl (first VCN first): CF-like
   result: OK and *r, OFAT_NTFS_END when not in it, EBAD when damaged. */
static int run_of(const ofat_u8 *rl, const ofat_u8 *end, ofat_u32 first, ofat_u32 vcn, struct ofat_ntfs_run *r)
{
    ofat_u32 cur = first, lcn = 0, len;
    ofat_s32 delta;
    unsigned ln, on, i;
    while (rl < end && *rl) {
        ln = *rl & 15; on = *rl >> 4;
        if (!ln || ln > 8 || on > 8 || rl + 1 + ln + on > end) return OFAT_NTFS_EBAD;
        ++rl;
        len = 0;
        for (i = 0; i < ln; ++i) if (i < 4) len |= (ofat_u32)rl[i] << (8 * i);
        rl += ln;
        if (on) {
            delta = 0;
            for (i = 0; i < on && i < 4; ++i) delta |= (ofat_s32)((ofat_u32)rl[i] << (8 * i));
            if (on < 4 && (rl[on - 1] & 0x80)) delta |= (ofat_s32)(NONE << (8 * on));
            rl += on;
            lcn += (ofat_u32)delta;
        }
        if (vcn >= cur && vcn - cur < len) {
            r->vcn = cur; r->len = len; r->lcn = on ? lcn : NONE;
            return OFAT_NTFS_OK;
        }
        cur += len;
    }
    return OFAT_NTFS_END;
}

/* the run of vcn in the non-resident attribute a */
static int attr_run(const ofat_u8 *a, ofat_u32 vcn, struct ofat_ntfs_run *r)
{
    ofat_u32 alen = rd32(a + 4);
    if (!a[8] || alen < 0x40) return OFAT_NTFS_EBAD;
    return run_of(a + rd16(a + 0x20), a + alen, rd32(a + 0x10), vcn, r);
}

/* sector s (512 bytes) of the non-resident attribute a -> *lba (NOBUF:
   sparse) */
static int attr_sector(const struct ofat_ntfs *v, const ofat_u8 *a, ofat_u32 s, ofat_lba *lba)
{
    struct ofat_ntfs_run r;
    ofat_u32 vcn = s >> v->cshift;
    int rc = attr_run(a, vcn, &r);
    if (rc) return rc == OFAT_NTFS_END ? OFAT_NTFS_EBAD : rc;
    if (r.lcn == NONE) { *lba = NOBUF; return OFAT_NTFS_OK; }
    *lba = ((ofat_lba)(r.lcn + (vcn - r.vcn)) << v->cshift) + (s & ((1UL << v->cshift) - 1));
    return OFAT_NTFS_OK;
}

/* ---- records ---- */

/* the update sequence fixups of a record or index block of size bytes */
static int fixup(ofat_u8 *b, unsigned size)
{
    unsigned ofs = rd16(b + 4), cnt = rd16(b + 6), i;
    if (cnt < 2 || cnt - 1 != size / 512 || ofs + 2 * cnt > 512) return OFAT_NTFS_EBAD;
    for (i = 1; i < cnt; ++i) {
        if (rd16(b + i * 512 - 2) != rd16(b + ofs)) return OFAT_NTFS_EBAD;
        b[i * 512 - 2] = b[ofs + 2 * i];
        b[i * 512 - 1] = b[ofs + 2 * i + 1];
    }
    return OFAT_NTFS_OK;
}

static int mft_sector(const struct ofat_ntfs *v, ofat_u32 s, ofat_lba *lba)
{
    ofat_u32 vcn = s >> v->cshift;
    unsigned i;
    for (i = 0; i < v->mft_nruns; ++i)
        if (vcn >= v->mft[i].vcn && vcn - v->mft[i].vcn < v->mft[i].len) {
            *lba = ((ofat_lba)(v->mft[i].lcn + (vcn - v->mft[i].vcn)) << v->cshift) + (s & ((1UL << v->cshift) - 1));
            return OFAT_NTFS_OK;
        }
    return OFAT_NTFS_EBAD;
}

/* MFT record no into b (*cached tells what it holds) */
static int load_rec(struct ofat_ntfs *v, ofat_u32 no, ofat_u8 *b, ofat_u32 *cached)
{
    unsigned n = v->rec_size / 512, i;
    ofat_u32 s = no * n;
    ofat_lba lba;
    int r;
    if (*cached == no) return OFAT_NTFS_OK;
    *cached = NONE;
    for (i = 0; i < n; ++i) {
        if ((r = mft_sector(v, s + i, &lba))) return r;
        if (v->buf_lba == lba) v->buf_lba = NOBUF;
        if (v->read(v->dev, v->start + lba, b + i * 512)) return OFAT_NTFS_EIO;
    }
    if (memcmp(b, "FILE", 4) || fixup(b, v->rec_size)) return OFAT_NTFS_EBAD;
    if (!(rd16(b + 0x16) & 1)) return OFAT_NTFS_END;            /* not in use */
    *cached = no;
    return OFAT_NTFS_OK;
}

/* The attribute of type (name "$I30" when i30, else none) in record b, or 0. */
static const ofat_u8 *find_attr(const struct ofat_ntfs *v, const ofat_u8 *b, ofat_u32 type, int i30)
{
    const ofat_u8 *a = b + rd16(b + 0x14), *end = b + v->rec_size;
    ofat_u32 t, len;
    while (a + 8 <= end) {
        t = rd32(a);
        if (t == AT_END) break;
        len = rd32(a + 4);
        if (len < 0x18 || len > (ofat_u32)(end - a)) break;
        if (t == type) {
            if (!i30 && !a[9]) return a;
            if (i30 && a[9] == 4 && (ofat_u32)rd16(a + 0x0A) + 8 <= len &&
                !memcmp(a + rd16(a + 0x0A), "$\0I\0" "3\0" "0\0", 8)) return a;
        }
        a += len;
    }
    return 0;
}

/* the value of a resident attribute, *len its length */
static const ofat_u8 *res_value(const ofat_u8 *a, ofat_u32 *len)
{
    *len = rd32(a + 0x10);
    if (a[8] || rd16(a + 0x14) + *len > rd32(a + 4)) return 0;
    return a + rd16(a + 0x14);
}

/* ---- times ---- */

/* NTFS time (100 ns from 1601, UTC) -> DOS date and time */
static void dos_time(const ofat_u8 *p, ofat_s16 tz, ofat_u16 *time, ofat_u16 *date)
{
    ofat_u32 secs, days, y, m, d;
    ofat_u64 t = (ofat_u64)rd32(p + 4) << 32 | rd32(p);
    t /= 10000000UL;                            /* seconds from 1601 */
    if (tz < 0) t -= (ofat_u32)-tz * 60UL; else t += (ofat_u32)tz * 60UL;   /* local time */
    if (t < 11960035200UL) { *time = 0; *date = 1 << 5 | 1; return; }   /* before 1980 */
    t -= 11644473600UL;                         /* from 1970 */
    secs = t > 0xFFFFFFFFUL ? 0xFFFFFFFFUL : (ofat_u32)t;
    days = secs / 86400; secs %= 86400;
    /* civil from days (Howard Hinnant), days from 1970-03-01 */
    days += 719468UL;
    {
        ofat_u32 era = days / 146097, doe = days - era * 146097;
        ofat_u32 yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
        ofat_u32 doy = doe - (365 * yoe + yoe / 4 - yoe / 100), mp = (5 * doy + 2) / 153;
        d = doy - (153 * mp + 2) / 5 + 1;
        m = mp < 10 ? mp + 3 : mp - 9;
        y = yoe + era * 400 + (m <= 2);
    }
    if (y < 1980) { *time = 0; *date = 1 << 5 | 1; return; }
    if (y > 2107) y = 2107;
    *date = (ofat_u16)((y - 1980) << 9 | m << 5 | d);
    *time = (ofat_u16)((secs / 3600) << 11 | (secs / 60 % 60) << 5 | (secs % 60) / 2);
}

/* ---- mounting ---- */

int ofat_ntfs_probe(const ofat_u8 OFAT_FAR *boot)
{
    return boot[3] == 'N' && boot[4] == 'T' && boot[5] == 'F' && boot[6] == 'S' &&
           boot[7] == ' ' && boot[510] == 0x55 && boot[511] == 0xAA;
}

/* the size of a record or index block from the boot sector byte c */
static ofat_u32 rec_bytes(ofat_s8 c, unsigned cshift)
{
    if (c < 0) return c < -20 ? 0 : 1UL << -c;
    return (ofat_u32)(ofat_u8)c << (cshift + 9);
}

static int stat_rec(struct ofat_ntfs *, ofat_u32, struct ofat_ntfs_ent *);

int ofat_ntfs_mount(struct ofat_ntfs *v, ofat_ntfs_read_fn read, void *dev, ofat_lba start,
                    const struct ofat_cp *cp, ofat_u8 *buf)
{
    const ofat_u8 *b, *a, *val;
    ofat_u32 spc, rs, is, len, vcn, alen;
    struct ofat_ntfs_run r;
    unsigned i;
    int rc;
    memset(v, 0, sizeof(*v));
    v->read = read; v->dev = dev; v->start = start; v->cp = cp;
    v->rec = buf; v->rec2 = buf + OFAT_NTFS_MAXREC; v->idx = buf + 2 * OFAT_NTFS_MAXREC;
    v->buf = buf + 3 * OFAT_NTFS_MAXREC;
    v->buf_lba = NOBUF; v->rec_no = v->rec2_no = v->idx_no = NONE; v->free = NONE;
    if (!(b = sector(v, 0))) return OFAT_NTFS_EIO;
    if (!ofat_ntfs_probe(b)) return OFAT_NTFS_EBAD;
    if (rd16(b + 0x0B) != 512) return OFAT_NTFS_ESIZE;
    spc = b[0x0D];
    if (spc > 0x80) spc = 1UL << (256 - spc);
    for (i = 0; i < 16 && (1UL << i) != spc; ++i) ;
    if (i == 16) return OFAT_NTFS_EBAD;
    v->cshift = (ofat_u8)i;
    rs = rec_bytes((ofat_s8)b[0x40], v->cshift);
    is = rec_bytes((ofat_s8)b[0x44], v->cshift);
    if (rs < 1024 || rs > OFAT_NTFS_MAXREC || (rs & 511) || is < 512 || is > OFAT_NTFS_MAXREC || (is & 511))
        return OFAT_NTFS_ESIZE;
    v->rec_size = (ofat_u16)rs; v->idx_size = (ofat_u16)is;
    v->clusters = (rd32(b + 0x2C) ? NONE : rd32(b + 0x28)) >> v->cshift;
    v->serial = rd32(b + 0x48);
    /* $MFT's first run from the boot sector, then its own runlist */
    v->mft[0].vcn = 0; v->mft[0].len = (rs + (512UL << v->cshift) - 1) >> (9 + v->cshift);
    v->mft[0].lcn = rd32(b + 0x30);
    if (rd32(b + 0x34)) return OFAT_NTFS_ESIZE;
    v->mft_nruns = 1;
    if ((rc = load_rec(v, 0, v->rec, &v->rec_no))) return rc == OFAT_NTFS_END ? OFAT_NTFS_EBAD : rc;
    if (!(a = find_attr(v, v->rec, AT_DATA, 0)) || !a[8]) return OFAT_NTFS_EBAD;
    alen = rd32(a + 0x18) + 1;                  /* highest VCN + 1 */
    v->mft_nruns = 0;
    for (vcn = 0; vcn < alen && v->mft_nruns < OFAT_NTFS_MFTRUNS; vcn = r.vcn + r.len) {
        if ((rc = attr_run(a, vcn, &r)) || r.lcn == NONE) return OFAT_NTFS_EBAD;
        v->mft[v->mft_nruns++] = r;
    }
    /* the label and the dirty flag ($Volume) */
    if (load_rec(v, 3, v->rec, &v->rec_no) == OFAT_NTFS_OK) {
        if ((a = find_attr(v, v->rec, AT_VNAME, 0)) && (val = res_value(a, &len))) {
            len /= 2;
            if (len > 32) len = 32;
            for (i = 0; i < len; ++i) v->label[i] = rd16(val + 2 * i);
            v->label_len = (ofat_u8)len;
        }
        if ((a = find_attr(v, v->rec, AT_VINFO, 0)) && (val = res_value(a, &len)) && len >= 12)
            v->dirty = (ofat_u8)(rd16(val + 10) & 1);
    }
    return OFAT_NTFS_OK;
}

void ofat_ntfs_rootdir(const struct ofat_ntfs *v, struct ofat_ntfs_file *f)
{
    (void)v;
    memset(f, 0, sizeof(*f));
    f->first = OFAT_NTFS_ROOT;
    f->root = 1;
}

/* ---- file data ---- */

/* the unnamed $DATA of record no covering vcn, in rec2 (through a resident
   attribute list): *a, or 0 and the sizes from the first extent */
static int data_attr(struct ofat_ntfs *v, ofat_u32 no, ofat_u32 vcn, const ofat_u8 **pa)
{
    const ofat_u8 *a, *l, *end, *val;
    ofat_u32 len, target = NONE;
    int rc;
    if ((rc = load_rec(v, no, v->rec2, &v->rec2_no))) return rc == OFAT_NTFS_END ? OFAT_NTFS_EBAD : rc;
    a = find_attr(v, v->rec2, AT_DATA, 0);
    if (a && (!a[8] || (vcn >= rd32(a + 0x10) && vcn <= rd32(a + 0x18)))) { *pa = a; return OFAT_NTFS_OK; }
    if (!(l = find_attr(v, v->rec2, AT_LIST, 0))) return a ? (*pa = a, OFAT_NTFS_OK) : OFAT_NTFS_EBAD;
    if (l[8]) {                                 /* non-resident: into idx */
        ofat_u32 i;
        ofat_lba lba;
        if (rd32(l + 0x34) || (len = rd32(l + 0x30)) > OFAT_NTFS_MAXREC) return OFAT_NTFS_ESIZE;
        v->idx_no = NONE;
        for (i = 0; i * 512 < len; ++i) {
            if ((rc = attr_sector(v, l, i, &lba))) return rc;
            if (lba == NOBUF) return OFAT_NTFS_EBAD;
            if (v->buf_lba == lba) v->buf_lba = NOBUF;
            if (v->read(v->dev, v->start + lba, v->idx + i * 512)) return OFAT_NTFS_EIO;
        }
        val = v->idx;
    } else if (!(val = res_value(l, &len))) return OFAT_NTFS_EBAD;
    for (l = val, end = val + len; l + 0x1A <= end && rd16(l + 4) >= 0x1A; l += rd16(l + 4)) {
        if (rd32(l) != AT_DATA || l[6]) continue;
        if (rd32(l + 8) > vcn) break;
        target = rd32(l + 0x10);
    }
    if (target == NONE) return OFAT_NTFS_EBAD;
    if (target == no) return a ? (*pa = a, OFAT_NTFS_OK) : OFAT_NTFS_EBAD;
    if ((rc = load_rec(v, target, v->rec2, &v->rec2_no))) return rc == OFAT_NTFS_END ? OFAT_NTFS_EBAD : rc;
    for (a = v->rec2 + rd16(v->rec2 + 0x14); a + 0x18 <= v->rec2 + v->rec_size && rd32(a) != AT_END; a += rd32(a + 4)) {
        if (rd32(a + 4) < 0x18) break;
        if (rd32(a) == AT_DATA && !a[9] && a[8] && vcn >= rd32(a + 0x10) && vcn <= rd32(a + 0x18)) {
            *pa = a; return OFAT_NTFS_OK;
        }
    }
    return OFAT_NTFS_EBAD;
}

int ofat_ntfs_read(struct ofat_ntfs *v, struct ofat_ntfs_file *f, ofat_fsize off, void OFAT_FAR *buf,
                   ofat_u16 len, ofat_u16 *done)
{
    ofat_u8 OFAT_FAR *out = (ofat_u8 OFAT_FAR *)buf;
    const ofat_u8 *a, *val, *s;
    ofat_u32 vlen, sec, vcn;
    struct ofat_ntfs_run r;
    unsigned in, n;
    ofat_lba lba;
    int rc;
    *done = 0;
    if (f->nofat == 2) return OFAT_NTFS_ECOMP;
    if (off >= f->size) return OFAT_NTFS_OK;
    if (len > f->size - off) len = (ofat_u16)(f->size - off);
    if (f->nofat) {                                 /* resident */
        if ((rc = data_attr(v, f->first, 0, &a))) return rc;
        if (!(val = res_value(a, &vlen)) || off + len > vlen) return OFAT_NTFS_EBAD;
        fmemcpy(out, val + (unsigned)off, len);
        *done = len;
        return OFAT_NTFS_OK;
    }
    while (len) {
        in = (unsigned)off & 511;
        n = 512 - in;
        if (n > len) n = len;
        if (off >= f->valid) {
            fmemset(out, 0, n);
        } else {
            if (f->valid - off < n) n = (unsigned)(f->valid - off);
            sec = (ofat_u32)(off >> 9);
            vcn = sec >> v->cshift;
            if (!f->rlen || vcn < f->idx || vcn - f->idx >= f->rlen) {
                if ((rc = data_attr(v, f->first, vcn, &a))) return rc;
                if (!a[8]) return OFAT_NTFS_EBAD;
                if ((rc = attr_run(a, vcn, &r))) return rc == OFAT_NTFS_END ? OFAT_NTFS_EBAD : rc;
                f->idx = r.vcn; f->clu = r.lcn; f->rlen = r.len;
            }
            if (f->clu == NONE) {
                fmemset(out, 0, n);
            } else {
                lba = ((ofat_lba)(f->clu + (vcn - f->idx)) << v->cshift) + (sec & ((1UL << v->cshift) - 1));
                if (!in && n == 512) {
                    if (v->buf_lba == lba) v->buf_lba = NOBUF;
                    if (v->read(v->dev, v->start + lba, out)) return OFAT_NTFS_EIO;
                } else {
                    if (!(s = sector(v, lba))) return OFAT_NTFS_EIO;
                    fmemcpy(out, s + in, n);
                }
            }
        }
        out += n; off += n; len -= n; *done += n;
    }
    return OFAT_NTFS_OK;
}

/* attributes, time and size of record no into e (rec2) */
static int stat_rec(struct ofat_ntfs *v, ofat_u32 no, struct ofat_ntfs_ent *e)
{
    const ofat_u8 *a, *val;
    ofat_u32 len, fl;
    int rc;
    struct ofat_ntfs_file *f = &e->file;
    memset(f, 0, sizeof(*f));
    f->first = no;
    if ((rc = load_rec(v, no, v->rec2, &v->rec2_no))) return rc;
    e->attr = 0; e->time = 0; e->date = 1 << 5 | 1;
    if ((a = find_attr(v, v->rec2, AT_STD, 0)) && (val = res_value(a, &len)) && len >= 36) {
        dos_time(val + 8, v->tz, &e->time, &e->date);
        e->attr = (ofat_u16)(rd32(val + 32) & (OFAT_NTFS_RDONLY | OFAT_NTFS_HIDDEN | OFAT_NTFS_SYSTEM | OFAT_NTFS_ARCH));
    }
    if (rd16(v->rec2 + 0x16) & 2) {             /* a directory */
        e->attr |= OFAT_NTFS_DIR;
        f->root = 1;
        return OFAT_NTFS_OK;
    }
    if ((rc = data_attr(v, no, 0, &a))) return rc;
    if (!a[8]) {
        if (!res_value(a, &len)) return OFAT_NTFS_EBAD;
        f->size = f->valid = len;
        f->nofat = 1;
        return OFAT_NTFS_OK;
    }
    fl = rd16(a + 0x0C);
    if (fl & 0x40FF) f->nofat = 2;              /* compressed, encrypted */
#if defined(__WATCOMC__) || defined(OFAT_NARROW)
    if (rd32(a + 0x34)) { f->big = 1; f->size = 0xFFFFFFFFUL; }
    else f->size = rd32(a + 0x30);
    f->valid = rd32(a + 0x3C) ? 0xFFFFFFFFUL : rd32(a + 0x38);
#else
    f->size = (ofat_fsize)rd32(a + 0x34) << 32 | rd32(a + 0x30);
    f->valid = (ofat_fsize)rd32(a + 0x3C) << 32 | rd32(a + 0x38);
#endif
    if (f->valid > f->size) f->valid = f->size;
    return OFAT_NTFS_OK;
}

/* ---- directories ---- */

#define IT_STAT 1                               /* attributes, size, time */
#define IT_DOS  2                               /* DOS-only names too */
#define UNUSED  2                               /* load_idx: a free index block */

/* the index block k of directory dir into v->idx; END past the last,
   1 when the block is not in use */
static int load_idx(struct ofat_ntfs *v, ofat_u32 dir, ofat_u32 k)
{
    const ofat_u8 *a, *bm, *val;
    ofat_u32 len, s, n = v->idx_size / 512, i, blocks;
    ofat_lba lba;
    int rc;
    if (v->idx_dir == dir && v->idx_no == k) return OFAT_NTFS_OK;
    if ((rc = load_rec(v, dir, v->rec, &v->rec_no))) return rc == OFAT_NTFS_END ? OFAT_NTFS_EBAD : rc;
    if (!(a = find_attr(v, v->rec, AT_IALLOC, 1)) || !a[8]) return OFAT_NTFS_END;
    blocks = rd32(a + 0x34) ? NONE : rd32(a + 0x30) / v->idx_size;
    if (k >= blocks) return OFAT_NTFS_END;
    if ((bm = find_attr(v, v->rec, AT_BITMAP, 1))) {
        if (!bm[8]) {
            if (!(val = res_value(bm, &len))) return OFAT_NTFS_EBAD;
            if (k / 8 < len && !(val[k / 8] >> (k & 7) & 1)) return UNUSED;
        } else {
            if ((rc = attr_sector(v, bm, k / 4096, &lba))) return rc;
            if (lba == NOBUF) return UNUSED;
            if (!(val = sector(v, lba))) return OFAT_NTFS_EIO;
            if (!(val[k / 8 % 512] >> (k & 7) & 1)) return UNUSED;
        }
    }
    v->idx_no = NONE;
    s = k * n;
    for (i = 0; i < n; ++i) {
        if ((rc = attr_sector(v, a, s + i, &lba))) return rc;
        if (lba == NOBUF) return OFAT_NTFS_EBAD;
        if (v->buf_lba == lba) v->buf_lba = NOBUF;
        if (v->read(v->dev, v->start + lba, v->idx + i * 512)) return OFAT_NTFS_EIO;
    }
    if (memcmp(v->idx, "INDX", 4) || fixup(v->idx, v->idx_size)) return OFAT_NTFS_EBAD;
    v->idx_dir = dir; v->idx_no = k;
    return OFAT_NTFS_OK;
}

static int iterate(struct ofat_ntfs *v, struct ofat_ntfs_file *dir, ofat_u32 *pos, struct ofat_ntfs_ent *e, unsigned how)
{
    const ofat_u8 *a, *base, *p, *key, *val;
    ofat_u32 blk, off, first, used, len, no;
    unsigned elen, klen, i;
    int rc;
    for (;;) {
        blk = *pos >> 16; off = *pos & 0xFFFF;
        if (!blk) {
            if ((rc = load_rec(v, dir->first, v->rec, &v->rec_no))) return rc == OFAT_NTFS_END ? OFAT_NTFS_EBAD : rc;
            if (!(a = find_attr(v, v->rec, AT_IROOT, 1)) || !(val = res_value(a, &len)) || len < 32)
                return OFAT_NTFS_EBAD;
            base = val + 16;
            if (rd32(base + 4) > len - 16) return OFAT_NTFS_EBAD;
        } else {
            rc = load_idx(v, dir->first, blk - 1);
            if (rc == UNUSED) { *pos = (blk + 1) << 16; continue; }
            if (rc) return rc;
            base = v->idx + 24;
            if (rd32(base + 4) > (ofat_u32)v->idx_size - 24) return OFAT_NTFS_EBAD;
        }
        first = rd32(base); used = rd32(base + 4);
        if (!off) off = first;
        if (off < first || off + 16 > used) return OFAT_NTFS_EBAD;
        p = base + off;
        elen = rd16(p + 8); klen = rd16(p + 10);
        if (elen < 16 || off + elen > used || 16 + klen > elen) return OFAT_NTFS_EBAD;
        if (rd16(p + 12) & 2) {                 /* the last entry of this node */
            *pos = (blk + 1) << 16;
            continue;
        }
        e->pos = blk << 16 | off;
        *pos = blk << 16 | (off + elen);
        no = rd32(p);
        key = p + 16;
        if (klen < 66 || 66u + 2u * key[64] > klen) return OFAT_NTFS_EBAD;
        if (no < 16 || rd16(p + 4)) continue;   /* system files */
        e->ns = key[65];
        if (e->ns == 2 && !(how & IT_DOS)) continue;
        e->name_len = key[64];
        for (i = 0; i < e->name_len; ++i) e->name[i] = rd16(key + 66 + 2 * i);
        if (how & IT_STAT) {
            rc = stat_rec(v, no, e);
            if (rc == OFAT_NTFS_END) continue;  /* a stale entry */
            if (rc) return rc;
        } else {
            memset(&e->file, 0, sizeof(e->file));
            e->file.first = no;
            e->attr = (rd32(key + 56) & F_DIR) ? OFAT_NTFS_DIR : 0;
        }
        return OFAT_NTFS_OK;
    }
}

int ofat_ntfs_dir_next(struct ofat_ntfs *v, struct ofat_ntfs_file *dir, ofat_u32 *pos, struct ofat_ntfs_ent *e)
{
    int rc = iterate(v, dir, pos, e, IT_STAT);
    return rc;
}

static struct ofat_ntfs_ent other;

int ofat_ntfs_short(struct ofat_ntfs *v, struct ofat_ntfs_file *d, const struct ofat_ntfs_ent *ent, char fcb[11])
{
    char basis[11], tail[8];
    ofat_u32 pos = 0, n = 1;
    unsigned len, keep, t;
    int r;
    if (!ofat_cp_basis(v->cp, ent->name, ent->name_len, fcb)) return OFAT_NTFS_OK;
    /* the 8.3 twin Windows made */
    for (;;) {
        r = iterate(v, d, &pos, &other, IT_DOS);
        if (r == OFAT_NTFS_END) break;
        if (r) return r;
        if (other.ns == 2 && other.file.first == ent->file.first) {
            ofat_cp_basis(v->cp, other.name, other.name_len, fcb);
            return OFAT_NTFS_OK;
        }
    }
    /* else the n-th entry with this basis in the directory gets "~n" */
    pos = 0;
    while (pos < ent->pos) {
        r = iterate(v, d, &pos, &other, 0);
        if (r == OFAT_NTFS_END) break;
        if (r) return r;
        if (other.pos < ent->pos && ofat_cp_basis(v->cp, other.name, other.name_len, basis) &&
            !memcmp(basis, fcb, 11)) ++n;
    }
    t = sizeof(tail);
    do { tail[--t] = (char)('0' + n % 10); n /= 10; } while (n);
    tail[--t] = '~';
    for (len = 8; len && fcb[len-1] == ' '; --len) ;
    keep = 8 - (sizeof(tail) - t);
    if (len > keep) len = keep;
    memcpy(fcb + len, tail + t, sizeof(tail) - t);
    return OFAT_NTFS_OK;
}

int ofat_ntfs_lookup(struct ofat_ntfs *v, struct ofat_ntfs_file *d, const char fcb[11], struct ofat_ntfs_ent *ent)
{
    char name[11];
    ofat_u32 pos = 0, next, twin = NONE;
    unsigned tilde;
    int r, lossy;
    for (tilde = 0; tilde < 8 && fcb[tilde] != '~'; ++tilde) ;
    for (;;) {
        r = iterate(v, d, &pos, ent, IT_DOS);
        if (r) return r;
        lossy = ofat_cp_basis(v->cp, ent->name, ent->name_len, name);
        if (ent->ns == 2) {                     /* a DOS name: its long name */
            if (lossy || memcmp(name, fcb, 11)) continue;
            twin = ent->file.first;
            break;
        }
        if (!lossy) {
            if (memcmp(name, fcb, 11)) continue;
        } else {
            if (tilde == 8) continue;           /* "~n" names have a "~" */
            next = pos;
            if ((r = ofat_ntfs_short(v, d, ent, name))) return r;
            pos = next;
            if (memcmp(name, fcb, 11)) continue;
        }
        break;
    }
    if (twin != NONE) {
        pos = 0;
        do {
            if ((r = iterate(v, d, &pos, ent, 0))) return r == OFAT_NTFS_END ? OFAT_NTFS_EBAD : r;
        } while (ent->file.first != twin);
    }
    next = ent->pos;
    r = stat_rec(v, ent->file.first, ent);
    ent->pos = next;
    return r == OFAT_NTFS_END ? OFAT_NTFS_EBAD : r;
}

int ofat_ntfs_free(struct ofat_ntfs *v, ofat_u32 *free_clusters)
{
    const ofat_u8 *a, *s;
    ofat_u32 c = 0, sec, nsec = (v->clusters + 4095) / 4096, bit, left;
    ofat_lba lba;
    unsigned i, b;
    int rc;
    if (v->free != NONE) { *free_clusters = v->free; return OFAT_NTFS_OK; }
    if ((rc = load_rec(v, 6, v->rec2, &v->rec2_no))) return rc == OFAT_NTFS_END ? OFAT_NTFS_EBAD : rc;
    if (!(a = find_attr(v, v->rec2, AT_DATA, 0)) || !a[8]) return OFAT_NTFS_EBAD;
    for (sec = 0; sec < nsec; ++sec) {
        if ((rc = attr_sector(v, a, sec, &lba))) return rc;
        left = v->clusters - sec * 4096;
        if (lba == NOBUF) { c += left > 4096 ? 4096 : left; continue; }
        if (!(s = sector(v, lba))) return OFAT_NTFS_EIO;
        for (i = 0; i < 512 && left; ++i)
            for (b = 0, bit = s[i]; b < 8 && left; ++b, --left)
                if (!(bit >> b & 1)) ++c;
    }
    v->free = c;
    *free_clusters = c;
    return OFAT_NTFS_OK;
}
