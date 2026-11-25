/* SPDX-License-Identifier: GPL-2.0-or-later */
/* NTFS reading: see include/ofs/ntfs.h.  The structures and offsets
   follow ntfs-3g's include/ntfs-3g/layout.h (GPL-2.0-or-later). */
#include <string.h>
#include "ofs/ntfs.h"

#ifdef __WATCOMC__
#include <string.h>
#define fmemcpy _fmemcpy
#define fmemset _fmemset
#else
#define fmemcpy memcpy
#define fmemset memset
#endif

#define NONE 0xFFFFFFFFUL
#define NOBUF ((ofs_lba)~(ofs_lba)0)

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

static ofs_u16 rd16(const ofs_u8 *p) { return (ofs_u16)(p[0] | p[1] << 8); }
static ofs_u32 rd32(const ofs_u8 *p) { return rd16(p) | (ofs_u32)rd16(p + 2) << 16; }

/* The core counts in blocks of 512 bytes (the update sequence fixups do
   too); a device sector of 1 << sshift bytes holds 1 << (sshift - 9) of
   them.  buf holds one device sector, buf_lba its number. */

/* block lba (512 bytes) through buf */
static const ofs_u8 *sector(struct ofs_ntfs *v, ofs_lba lba)
{
    unsigned sh = v->sshift - 9;
    ofs_lba dev = lba >> sh;
    if (v->buf_lba != dev) {
        v->buf_lba = NOBUF;
        if (v->read(v->dev, v->start + dev, v->buf)) return 0;
        v->buf_lba = dev;
    }
    return v->buf + ((unsigned)(lba & ((1u << sh) - 1)) << 9);
}

/* block lba into b: straight from the device when its sectors are blocks */
static int blk_read(struct ofs_ntfs *v, ofs_lba lba, ofs_u8 OFS_FAR *b)
{
    const ofs_u8 *s;
    if (v->sshift == 9) {
        if (v->buf_lba == lba) v->buf_lba = NOBUF;
        return v->read(v->dev, v->start + lba, b) ? OFS_NTFS_EIO : OFS_NTFS_OK;
    }
    if (!(s = sector(v, lba))) return OFS_NTFS_EIO;
    fmemcpy(b, s, 512);
    return OFS_NTFS_OK;
}

/* ---- runlists ---- */

/* The run of VCN vcn in the runlist at rl (first VCN first): CF-like
   result: OK and *r, OFS_NTFS_END when not in it, EBAD when damaged. */
static int run_of(const ofs_u8 *rl, const ofs_u8 *end, ofs_u32 first, ofs_u32 vcn, struct ofs_ntfs_run *r)
{
    ofs_u32 cur = first, lcn = 0, len;
    ofs_s32 delta;
    unsigned ln, on, i;
    while (rl < end && *rl) {
        ln = *rl & 15; on = *rl >> 4;
        if (!ln || ln > 8 || on > 8 || rl + 1 + ln + on > end) return OFS_NTFS_EBAD;
        ++rl;
        len = 0;
        for (i = 0; i < ln; ++i) if (i < 4) len |= (ofs_u32)rl[i] << (8 * i);
        rl += ln;
        if (on) {
            delta = 0;
            for (i = 0; i < on && i < 4; ++i) delta |= (ofs_s32)((ofs_u32)rl[i] << (8 * i));
            if (on < 4 && (rl[on - 1] & 0x80)) delta |= (ofs_s32)(NONE << (8 * on));
            rl += on;
            lcn += (ofs_u32)delta;
        }
        if (vcn >= cur && vcn - cur < len) {
            r->vcn = cur; r->len = len; r->lcn = on ? lcn : NONE;
            return OFS_NTFS_OK;
        }
        cur += len;
    }
    return OFS_NTFS_END;
}

/* the run of vcn in the non-resident attribute a */
static int attr_run(const ofs_u8 *a, ofs_u32 vcn, struct ofs_ntfs_run *r)
{
    ofs_u32 alen = rd32(a + 4);
    if (!a[8] || alen < 0x40) return OFS_NTFS_EBAD;
    return run_of(a + rd16(a + 0x20), a + alen, rd32(a + 0x10), vcn, r);
}

/* sector s (512 bytes) of the non-resident attribute a -> *lba (NOBUF:
   sparse) */
static int attr_sector(const struct ofs_ntfs *v, const ofs_u8 *a, ofs_u32 s, ofs_lba *lba)
{
    struct ofs_ntfs_run r;
    ofs_u32 vcn = s >> v->cshift;
    int rc = attr_run(a, vcn, &r);
    if (rc) return rc == OFS_NTFS_END ? OFS_NTFS_EBAD : rc;
    if (r.lcn == NONE) { *lba = NOBUF; return OFS_NTFS_OK; }
    *lba = ((ofs_lba)(r.lcn + (vcn - r.vcn)) << v->cshift) + (s & ((1UL << v->cshift) - 1));
    return OFS_NTFS_OK;
}

/* ---- records ---- */

/* the update sequence fixups of a record or index block of size bytes */
static int fixup(ofs_u8 *b, unsigned size)
{
    unsigned ofs = rd16(b + 4), cnt = rd16(b + 6), i;
    if (cnt < 2 || cnt - 1 != size / 512 || ofs + 2 * cnt > 512) return OFS_NTFS_EBAD;
    for (i = 1; i < cnt; ++i) {
        if (rd16(b + i * 512 - 2) != rd16(b + ofs)) return OFS_NTFS_EBAD;
        b[i * 512 - 2] = b[ofs + 2 * i];
        b[i * 512 - 1] = b[ofs + 2 * i + 1];
    }
    return OFS_NTFS_OK;
}

static int mft_sector(struct ofs_ntfs *v, ofs_u32 s, ofs_lba *lba);
static int attr_is(const ofs_u8 *a, ofs_u32 len, ofs_u32 type, int i30, ofs_u32 vcn);
static const ofs_u8 *find_attr(const struct ofs_ntfs *v, const ofs_u8 *b, ofs_u32 type, int i30);
static const ofs_u8 *res_value(const ofs_u8 *a, ofs_u32 *len);

/* record no of $MFT into idx through the runs known so far (for
   mft_sector: no new lookups from here) */
static int mft_raw(struct ofs_ntfs *v, ofs_u32 no)
{
    unsigned n = v->rec_size / 512, i;
    ofs_lba lba;
    int r;
    v->idx_no = NONE;
    for (i = 0; i < n; ++i) {
        if ((r = mft_sector(v, no * n + i, &lba))) return r;
        if (blk_read(v, lba, v->idx + i * 512)) return OFS_NTFS_EIO;
    }
    if (memcmp(v->idx, "FILE", 4) || fixup(v->idx, v->rec_size) || !(rd16(v->idx + 0x16) & 1))
        return OFS_NTFS_EBAD;
    return OFS_NTFS_OK;
}

/* the run of $MFT's data with vcn from its runlist (record 0, or the
   record its attribute list names), into the cache */
static int mft_lookup(struct ofs_ntfs *v, ofs_u32 vcn)
{
    const ofs_u8 *a, *l, *end, *val, *rend;
    ofs_u32 alen, len, target = NONE;
    struct ofs_ntfs_run r;
    int rc, pass;
    for (pass = 0; pass < 2; ++pass) {
        if ((rc = mft_raw(v, pass ? target : 0))) return rc;
        rend = v->idx + v->rec_size;
        for (a = v->idx + rd16(v->idx + 0x14); a + 0x18 <= rend && rd32(a) != AT_END; a += alen) {
            alen = rd32(a + 4);
            if (alen < 0x18 || alen > (ofs_u32)(rend - a)) return OFS_NTFS_EBAD;
            if (attr_is(a, alen, AT_DATA, 0, vcn) && a[8]) {
                if ((rc = attr_run(a, vcn, &r))) return rc == OFS_NTFS_END ? OFS_NTFS_EBAD : rc;
                if (r.lcn == NONE) return OFS_NTFS_EBAD;
                if (v->mft_nruns < OFS_NTFS_MFTRUNS) v->mft[v->mft_nruns++] = r;
                else {
                    v->mft[v->mft_next] = r;    /* the first run stays */
                    if (++v->mft_next == OFS_NTFS_MFTRUNS) v->mft_next = 1;
                }
                return OFS_NTFS_OK;
            }
        }
        /* not in this record: the attribute list of record 0 */
        if (pass || !(l = find_attr(v, v->idx, AT_LIST, 0))) break;
        if (l[8]) {                             /* non-resident: into idx */
            ofs_u8 la[128];
            ofs_u32 i;
            ofs_lba lba;
            if ((alen = rd32(l + 4)) > sizeof(la) || rd32(l + 0x34) || (len = rd32(l + 0x30)) > OFS_NTFS_MAXREC)
                return OFS_NTFS_ESIZE;
            memcpy(la, l, alen);
            for (i = 0; i * 512 < len; ++i) {
                if ((rc = attr_sector(v, la, i, &lba))) return rc;
                if (lba == NOBUF) return OFS_NTFS_EBAD;
                if (blk_read(v, lba, v->idx + i * 512)) return OFS_NTFS_EIO;
            }
            val = v->idx;
        } else if (!(val = res_value(l, &len))) break;
        for (l = val, end = val + len; l + 0x1A <= end && rd16(l + 4) >= 0x1A && l + rd16(l + 4) <= end; l += rd16(l + 4)) {
            if (rd32(l) != AT_DATA || l[6]) continue;
            if (rd32(l + 0x0C) || rd32(l + 8) > vcn) break;
            target = rd32(l + 0x10);
        }
        if (target == NONE || !target) break;
    }
    return OFS_NTFS_EBAD;
}

static int mft_sector(struct ofs_ntfs *v, ofs_u32 s, ofs_lba *lba)
{
    ofs_u32 vcn = s >> v->cshift;
    unsigned i, tries;
    int rc;
    for (tries = 0; tries < 2; ++tries) {
        for (i = 0; i < v->mft_nruns; ++i)
            if (vcn >= v->mft[i].vcn && vcn - v->mft[i].vcn < v->mft[i].len) {
                *lba = ((ofs_lba)(v->mft[i].lcn + (vcn - v->mft[i].vcn)) << v->cshift) + (s & ((1UL << v->cshift) - 1));
                return OFS_NTFS_OK;
            }
        if (tries || v->mft_busy) break;
        v->mft_busy = 1;
        rc = mft_lookup(v, vcn);
        v->mft_busy = 0;
        if (rc) return rc;
    }
    return OFS_NTFS_EBAD;
}

/* MFT record no into b (*cached tells what it holds) */
static int load_rec(struct ofs_ntfs *v, ofs_u32 no, ofs_u8 *b, ofs_u32 *cached)
{
    unsigned n = v->rec_size / 512, i;
    ofs_u32 s = no * n;
    ofs_lba lba;
    int r;
    if (*cached == no) return OFS_NTFS_OK;
    *cached = NONE;
    for (i = 0; i < n; ++i) {
        if ((r = mft_sector(v, s + i, &lba))) return r;
        if (blk_read(v, lba, b + i * 512)) return OFS_NTFS_EIO;
    }
    if (memcmp(b, "FILE", 4) || fixup(b, v->rec_size)) return OFS_NTFS_EBAD;
    if (!(rd16(b + 0x16) & 1)) return OFS_NTFS_END;            /* not in use */
    *cached = no;
    return OFS_NTFS_OK;
}

/* The attribute of type (name "$I30" when i30, else none) in record b, or 0. */
static const ofs_u8 *find_attr(const struct ofs_ntfs *v, const ofs_u8 *b, ofs_u32 type, int i30)
{
    const ofs_u8 *a = b + rd16(b + 0x14), *end = b + v->rec_size;
    ofs_u32 t, len;
    while (a + 8 <= end) {
        t = rd32(a);
        if (t == AT_END) break;
        len = rd32(a + 4);
        if (len < 0x18 || len > (ofs_u32)(end - a)) break;
        if (t == type) {
            if (!i30 && !a[9]) return a;
            if (i30 && a[9] == 4 && (ofs_u32)rd16(a + 0x0A) + 8 <= len &&
                !memcmp(a + rd16(a + 0x0A), "$\0I\0" "3\0" "0\0", 8)) return a;
        }
        a += len;
    }
    return 0;
}

/* the value of a resident attribute, *len its length */
static const ofs_u8 *res_value(const ofs_u8 *a, ofs_u32 *len)
{
    ofs_u32 alen = rd32(a + 4), voff = rd16(a + 0x14);
    *len = rd32(a + 0x10);
    if (a[8] || voff > alen || *len > alen - voff) return 0;    /* no wrap */
    return a + rd16(a + 0x14);
}

/* ---- times ---- */

/* NTFS time (100 ns from 1601, UTC) -> DOS date and time */
static void dos_time(const ofs_u8 *p, ofs_s16 tz, ofs_u16 *time, ofs_u16 *date)
{
    ofs_u32 secs, days, y, m, d;
    ofs_u64 t = (ofs_u64)rd32(p + 4) << 32 | rd32(p);
    t /= 10000000UL;                            /* seconds from 1601 */
    if (tz < 0) t -= (ofs_u32)-tz * 60UL; else t += (ofs_u32)tz * 60UL;   /* local time */
    if (t < 11960035200UL) { *time = 0; *date = 1 << 5 | 1; return; }   /* before 1980 */
    t -= 11644473600UL;                         /* from 1970 */
    secs = t > 0xFFFFFFFFUL ? 0xFFFFFFFFUL : (ofs_u32)t;
    days = secs / 86400; secs %= 86400;
    /* civil from days (Howard Hinnant), days from 1970-03-01 */
    days += 719468UL;
    {
        ofs_u32 era = days / 146097, doe = days - era * 146097;
        ofs_u32 yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
        ofs_u32 doy = doe - (365 * yoe + yoe / 4 - yoe / 100), mp = (5 * doy + 2) / 153;
        d = doy - (153 * mp + 2) / 5 + 1;
        m = mp < 10 ? mp + 3 : mp - 9;
        y = yoe + era * 400 + (m <= 2);
    }
    if (y < 1980) { *time = 0; *date = 1 << 5 | 1; return; }
    if (y > 2107) y = 2107;
    *date = (ofs_u16)((y - 1980) << 9 | m << 5 | d);
    *time = (ofs_u16)((secs / 3600) << 11 | (secs / 60 % 60) << 5 | (secs % 60) / 2);
}

/* ---- mounting ---- */

int ofs_ntfs_probe(const ofs_u8 OFS_FAR *boot)
{
    return boot[3] == 'N' && boot[4] == 'T' && boot[5] == 'F' && boot[6] == 'S' &&
           boot[7] == ' ' && boot[510] == 0x55 && boot[511] == 0xAA;
}

/* the size of a record or index block from the boot sector byte c */
static ofs_u32 rec_bytes(ofs_s8 c, unsigned cshift)
{
    if (c < 0) return c < -20 ? 0 : 1UL << -c;
    return (ofs_u32)(ofs_u8)c << (cshift + 9);
}

static int stat_rec(struct ofs_ntfs *, ofs_u32, struct ofs_ntfs_ent *);

int ofs_ntfs_mount(struct ofs_ntfs *v, ofs_ntfs_read_fn read, void *dev, ofs_lba start,
                    unsigned sshift, const struct ofs_cp *cp, ofs_u8 *buf)
{
    const ofs_u8 *b, *a, *val;
    ofs_u32 spc, rs, is, len, vcn, alen;
    struct ofs_ntfs_run r;
    unsigned i;
    int rc;
    memset(v, 0, sizeof(*v));
    v->read = read; v->dev = dev; v->start = start; v->cp = cp;
    v->sshift = (ofs_u8)(sshift ? sshift : 9);
    if (v->sshift < 9 || (1UL << v->sshift) > OFS_NTFS_MAXSECT) return OFS_NTFS_ESIZE;
    v->rec = buf; v->rec2 = buf + OFS_NTFS_MAXREC; v->idx = buf + 2 * OFS_NTFS_MAXREC;
    v->buf = buf + 3 * OFS_NTFS_MAXREC;
    v->buf_lba = NOBUF; v->rec_no = v->rec2_no = v->idx_no = NONE; v->free = NONE;
    if (!(b = sector(v, 0))) return OFS_NTFS_EIO;
    if (!ofs_ntfs_probe(b)) return OFS_NTFS_EBAD;
    if (rd16(b + 0x0B) != 1u << v->sshift) return OFS_NTFS_ESIZE;
    spc = b[0x0D];
    if (spc > 0x80) spc = 1UL << (256 - spc);
    for (i = 0; i < 16 && (1UL << i) != spc; ++i) ;
    if (i == 16) return OFS_NTFS_EBAD;
    v->cshift = (ofs_u8)(i + v->sshift - 9);   /* blocks per cluster */
    rs = rec_bytes((ofs_s8)b[0x40], v->cshift);
    is = rec_bytes((ofs_s8)b[0x44], v->cshift);
    if (rs < 1024 || rs > OFS_NTFS_MAXREC || (rs & 511) || is < 512 || is > OFS_NTFS_MAXREC || (is & 511))
        return OFS_NTFS_ESIZE;
    v->rec_size = (ofs_u16)rs; v->idx_size = (ofs_u16)is;
    v->clusters = (rd32(b + 0x2C) ? NONE : rd32(b + 0x28)) >> i;   /* device sectors */
    v->serial = rd32(b + 0x48);
    /* $MFT's first run from the boot sector, then its own runlist */
    v->mft[0].vcn = 0; v->mft[0].len = (rs + (512UL << v->cshift) - 1) >> (9 + v->cshift);
    v->mft[0].lcn = rd32(b + 0x30);
    if (rd32(b + 0x34)) return OFS_NTFS_ESIZE;
    v->mft_nruns = 1; v->mft_next = 1;
    if ((rc = load_rec(v, 0, v->rec, &v->rec_no))) return rc == OFS_NTFS_END ? OFS_NTFS_EBAD : rc;
    if (!(a = find_attr(v, v->rec, AT_DATA, 0)) || !a[8]) return OFS_NTFS_EBAD;
    alen = rd32(a + 0x18) + 1;                  /* highest VCN + 1 */
    v->mft_nruns = 0;
    for (vcn = 0; vcn < alen && v->mft_nruns < OFS_NTFS_MFTRUNS; vcn = r.vcn + r.len) {
        if ((rc = attr_run(a, vcn, &r)) || r.lcn == NONE) return OFS_NTFS_EBAD;
        v->mft[v->mft_nruns++] = r;
    }
    /* the label and the dirty flag ($Volume) */
    if (load_rec(v, 3, v->rec, &v->rec_no) == OFS_NTFS_OK) {
        if ((a = find_attr(v, v->rec, AT_VNAME, 0)) && (val = res_value(a, &len))) {
            len /= 2;
            if (len > 32) len = 32;
            for (i = 0; i < len; ++i) v->label[i] = rd16(val + 2 * i);
            v->label_len = (ofs_u8)len;
        }
        if ((a = find_attr(v, v->rec, AT_VINFO, 0)) && (val = res_value(a, &len)) && len >= 12)
            v->dirty = (ofs_u8)(rd16(val + 10) & 1);
    }
    return OFS_NTFS_OK;
}

void ofs_ntfs_rootdir(const struct ofs_ntfs *v, struct ofs_ntfs_file *f)
{
    (void)v;
    memset(f, 0, sizeof(*f));
    f->first = OFS_NTFS_ROOT;
    f->root = 1;
}

/* ---- file data ---- */

/* Does the attribute a of a record match type, name ("$I30" when i30, else
   none) and, when non-resident, cover vcn? */
static int attr_is(const ofs_u8 *a, ofs_u32 len, ofs_u32 type, int i30, ofs_u32 vcn)
{
    if (rd32(a) != type) return 0;
    if (i30 ? a[9] != 4 || (ofs_u32)rd16(a + 0x0A) + 8 > len ||
              memcmp(a + rd16(a + 0x0A), "$\0I\0" "3\0" "0\0", 8) : a[9] != 0) return 0;
    return !a[8] || (len >= 0x40 && vcn >= rd32(a + 0x10) && vcn <= rd32(a + 0x18));
}

/* The attribute of type (named "$I30" when i30) of record no that covers
   vcn (resident ones: any vcn), through the attribute list when the record
   has one (a non-resident list is read into idx): *pa points into b, which
   then holds the record it is in (*cached its number).  END when the
   record has no such attribute. */
static int ext_attr(struct ofs_ntfs *v, ofs_u32 no, ofs_u32 type, int i30, ofs_u32 vcn,
                    ofs_u8 *b, ofs_u32 *cached, const ofs_u8 **pa)
{
    const ofs_u8 *a, *l, *end, *val, *rend;
    ofs_u32 len, target = NONE, alen;
    int rc, any = 0;
    if ((rc = load_rec(v, no, b, cached))) return rc == OFS_NTFS_END ? OFS_NTFS_EBAD : rc;
    for (a = b + rd16(b + 0x14), rend = b + v->rec_size; a + 0x18 <= rend && rd32(a) != AT_END; a += alen) {
        alen = rd32(a + 4);
        if (alen < 0x18 || alen > (ofs_u32)(rend - a)) return OFS_NTFS_EBAD;
        if (attr_is(a, alen, type, i30, vcn)) { *pa = a; return OFS_NTFS_OK; }
    }
    if (!(l = find_attr(v, b, AT_LIST, 0))) return OFS_NTFS_END;
    if (l[8]) {                                 /* non-resident: into idx */
        ofs_u32 i;
        ofs_lba lba;
        if (rd32(l + 0x34) || (len = rd32(l + 0x30)) > OFS_NTFS_MAXREC) return OFS_NTFS_ESIZE;
        v->idx_no = NONE;
        for (i = 0; i * 512 < len; ++i) {
            if ((rc = attr_sector(v, l, i, &lba))) return rc;
            if (lba == NOBUF) return OFS_NTFS_EBAD;
            if (blk_read(v, lba, v->idx + i * 512)) return OFS_NTFS_EIO;
        }
        val = v->idx;
    } else if (!(val = res_value(l, &len))) return OFS_NTFS_EBAD;
    /* the last entry of the type that starts at or before vcn */
    for (l = val, end = val + len; l + 0x1A <= end && rd16(l + 4) >= 0x1A && l + rd16(l + 4) <= end; l += rd16(l + 4)) {
        if (rd32(l) != type) continue;
        if (i30 ? l[6] != 4 || l + l[7] + 8 > end || memcmp(l + l[7], "$\0I\0" "3\0" "0\0", 8) : l[6] != 0)
            continue;
        any = 1;
        if (rd32(l + 0x0C) || rd32(l + 8) > vcn) break;
        target = rd32(l + 0x10);
    }
    if (target == NONE) return any ? OFS_NTFS_EBAD : OFS_NTFS_END;
    if (target == no) return OFS_NTFS_EBAD;   /* it was not in the record */
    if ((rc = load_rec(v, target, b, cached))) return rc == OFS_NTFS_END ? OFS_NTFS_EBAD : rc;
    for (a = b + rd16(b + 0x14); a + 0x18 <= rend && rd32(a) != AT_END; a += alen) {
        alen = rd32(a + 4);
        if (alen < 0x18 || alen > (ofs_u32)(rend - a)) return OFS_NTFS_EBAD;
        if (attr_is(a, alen, type, i30, vcn)) { *pa = a; return OFS_NTFS_OK; }
    }
    return OFS_NTFS_EBAD;
}

/* the unnamed $DATA of record no covering vcn, in rec2 */
static int data_attr(struct ofs_ntfs *v, ofs_u32 no, ofs_u32 vcn, const ofs_u8 **pa)
{
    int rc = ext_attr(v, no, AT_DATA, 0, vcn, v->rec2, &v->rec2_no, pa);
    return rc == OFS_NTFS_END ? OFS_NTFS_EBAD : rc;
}

/* ---- compressed files (LZNT1) ----
   A compressed $DATA is cut into compression units (CU) of 16 clusters
   (clusters of 4 KB at most).  A unit with all its clusters allocated is
   stored plain, one without any is zeros, else its first clusters hold
   the LZNT1 stream of the unit: chunks of 4096 bytes, each a 16-bit header
   (bits 0-11: its size in bytes - 3, bit 15: compressed; 0: no more
   chunks, the rest is zeros) and its data.  A chunk refers to itself only,
   so one at a time is decompressed, into idx.  idx_dir is then the file's
   record and idx_no the chunk; a file is never a directory, so load_idx
   does not take it for an index block (and every other user of idx sets
   idx_no to NONE). */

#define CU_CLU   16
#define CHUNK    4096

struct cstream {
    struct ofs_ntfs *v;
    const ofs_u32 *lcn;                        /* the unit's clusters */
    ofs_u32 pos, end;                          /* bytes in the stream */
    const ofs_u8 *s;                           /* sector spos of it */
    ofs_u32 spos;
};

/* the next byte of the stream; -1 at its end, -2 read error */
static int cbyte(struct cstream *c)
{
    ofs_u32 sec;
    ofs_lba lba;
    if (c->pos >= c->end) return -1;
    sec = c->pos >> 9;
    if (!c->s || c->spos != sec) {
        lba = ((ofs_lba)c->lcn[sec >> c->v->cshift] << c->v->cshift) + (sec & ((1UL << c->v->cshift) - 1));
        if (!(c->s = sector(c->v, lba))) return -2;
        c->spos = sec;
    }
    return c->s[(unsigned)(c->pos++ & 511)];
}

#define CBYTE(c, b) if (((b) = cbyte(c)) < 0) return (b) == -2 ? OFS_NTFS_EIO : OFS_NTFS_EBAD

/* the chunk with header hdr that ends at cend into out (4096 bytes) */
static int lznt1(struct cstream *c, ofs_u8 *out, unsigned hdr, ofs_u32 cend)
{
    unsigned dst = 0, tag, bit, tok, lg, i, pt, n;
    int b, b2;
    if (!(hdr & 0x8000)) {                      /* stored */
        if ((hdr & 0xFFF) != CHUNK - 1) return OFS_NTFS_EBAD;
        for (; dst < CHUNK; ++dst) { CBYTE(c, b); out[dst] = (ofs_u8)b; }
        return OFS_NTFS_OK;
    }
    while (c->pos < cend && dst < CHUNK) {
        CBYTE(c, b);
        tag = (unsigned)b;
        for (bit = 0; bit < 8 && c->pos < cend && dst < CHUNK; ++bit, tag >>= 1) {
            if (!(tag & 1)) { CBYTE(c, b); out[dst++] = (ofs_u8)b; continue; }
            CBYTE(c, b); CBYTE(c, b2);
            tok = (unsigned)b | (unsigned)b2 << 8;
            if (!dst) return OFS_NTFS_EBAD;
            /* the more is decompressed, the more bits the offset gets */
            for (i = dst - 1, lg = 0; i >= 0x10; i >>= 1) ++lg;
            pt = (tok >> (12 - lg)) + 1;
            n = (tok & (0xFFFu >> lg)) + 3;
            if (pt > dst) return OFS_NTFS_EBAD;
            for (; n && dst < CHUNK; --n, ++dst) out[dst] = out[dst - pt];
        }
    }
    if (dst < CHUNK) memset(out + dst, 0, CHUNK - dst);
    return OFS_NTFS_OK;
}

/* chunk k (the bytes from k * 4096) of the compressed file f into idx */
static int comp_chunk(struct ofs_ntfs *v, struct ofs_ntfs_file *f, ofs_u32 k)
{
    ofs_u32 lcn[CU_CLU], first, vcn, sec;
    unsigned i, nalloc, per_cu = 2u << v->cshift, j;
    struct ofs_ntfs_run r;
    struct cstream c;
    const ofs_u8 *a;
    ofs_lba lba;
    int rc, b, b2;
    unsigned hdr;
    ofs_u32 chunk = k;
    if (v->idx_dir == f->first && v->idx_no == k) return OFS_NTFS_OK;
    first = k / per_cu * CU_CLU;
    for (i = 0; i < CU_CLU; ++i) {
        vcn = first + i;
        if (!f->rlen || vcn < f->idx || vcn - f->idx >= f->rlen) {
            if ((rc = data_attr(v, f->first, vcn, &a))) return rc;
            if (!a[8]) return OFS_NTFS_EBAD;
            if ((rc = attr_run(a, vcn, &r))) return rc == OFS_NTFS_END ? OFS_NTFS_EBAD : rc;
            f->idx = r.vcn; f->clu = r.lcn; f->rlen = r.len;
        }
        lcn[i] = f->clu == NONE ? NONE : f->clu + (vcn - f->idx);
    }
    for (nalloc = 0; nalloc < CU_CLU && lcn[nalloc] != NONE; ++nalloc) ;
    for (i = nalloc; i < CU_CLU; ++i) if (lcn[i] != NONE) return OFS_NTFS_EBAD;
    v->idx_no = NONE;
    k %= per_cu;                                /* the chunk in the unit */
    if (nalloc == CU_CLU) {                     /* stored plain */
        for (j = 0; j < CHUNK / 512; ++j) {
            sec = k * (CHUNK / 512) + j;
            lba = ((ofs_lba)lcn[sec >> v->cshift] << v->cshift) + (sec & ((1UL << v->cshift) - 1));
            if (blk_read(v, lba, v->idx + j * 512)) return OFS_NTFS_EIO;
        }
    } else {
        c.v = v; c.lcn = lcn; c.pos = 0; c.end = (ofs_u32)nalloc << (v->cshift + 9); c.s = 0; c.spos = 0;
        for (;;) {
            if (!nalloc || c.pos + 2 > c.end) hdr = 0;
            else { CBYTE(&c, b); CBYTE(&c, b2); hdr = (unsigned)b | (unsigned)b2 << 8; }
            if (!hdr) { memset(v->idx, 0, CHUNK); break; }
            if (c.pos + (hdr & 0xFFF) + 1 > c.end) return OFS_NTFS_EBAD;
            if (!k) {
                if ((rc = lznt1(&c, v->idx, hdr, c.pos + (hdr & 0xFFF) + 1))) return rc;
                break;
            }
            c.pos += (hdr & 0xFFF) + 1;
            --k;
        }
    }
    v->idx_dir = f->first; v->idx_no = chunk;
    return OFS_NTFS_OK;
}

int ofs_ntfs_read(struct ofs_ntfs *v, struct ofs_ntfs_file *f, ofs_fsize off, void OFS_FAR *buf,
                   ofs_u16 len, ofs_u16 *done)
{
    ofs_u8 OFS_FAR *out = (ofs_u8 OFS_FAR *)buf;
    const ofs_u8 *a, *val, *s;
    ofs_u32 vlen, sec, vcn;
    struct ofs_ntfs_run r;
    unsigned in, n;
    ofs_lba lba;
    int rc;
    *done = 0;
    if (f->nofat == 2) return OFS_NTFS_ECOMP;
    if (off >= f->size) return OFS_NTFS_OK;
    if (len > f->size - off) len = (ofs_u16)(f->size - off);
    if (f->nofat == 1) {                            /* resident */
        if ((rc = data_attr(v, f->first, 0, &a))) return rc;
        if (!(val = res_value(a, &vlen)) || off + len > vlen) return OFS_NTFS_EBAD;
        fmemcpy(out, val + (unsigned)off, len);
        *done = len;
        return OFS_NTFS_OK;
    }
    while (len) {
        in = (unsigned)off & (f->nofat == 3 ? CHUNK - 1 : 511);
        n = (f->nofat == 3 ? CHUNK : 512) - in;
        if (n > len) n = len;
        if (off >= f->valid) {
            fmemset(out, 0, n);
        } else if (f->nofat == 3) {                 /* compressed */
            if (f->valid - off < n) n = (unsigned)(f->valid - off);
            if ((rc = comp_chunk(v, f, (ofs_u32)(off >> 12)))) return rc;
            fmemcpy(out, v->idx + in, n);
        } else {
            if (f->valid - off < n) n = (unsigned)(f->valid - off);
            sec = (ofs_u32)(off >> 9);
            vcn = sec >> v->cshift;
            if (!f->rlen || vcn < f->idx || vcn - f->idx >= f->rlen) {
                if ((rc = data_attr(v, f->first, vcn, &a))) return rc;
                if (!a[8]) return OFS_NTFS_EBAD;
                if ((rc = attr_run(a, vcn, &r))) return rc == OFS_NTFS_END ? OFS_NTFS_EBAD : rc;
                f->idx = r.vcn; f->clu = r.lcn; f->rlen = r.len;
            }
            if (f->clu == NONE) {
                fmemset(out, 0, n);
            } else {
                lba = ((ofs_lba)(f->clu + (vcn - f->idx)) << v->cshift) + (sec & ((1UL << v->cshift) - 1));
                if (!in && n == 512) {
                    if (blk_read(v, lba, out)) return OFS_NTFS_EIO;
                } else {
                    if (!(s = sector(v, lba))) return OFS_NTFS_EIO;
                    fmemcpy(out, s + in, n);
                }
            }
        }
        out += n; off += n; len -= n; *done += n;
    }
    return OFS_NTFS_OK;
}

/* attributes, time and size of record no into e (rec2) */
static int stat_rec(struct ofs_ntfs *v, ofs_u32 no, struct ofs_ntfs_ent *e)
{
    const ofs_u8 *a, *val;
    ofs_u32 len, fl;
    int rc;
    struct ofs_ntfs_file *f = &e->file;
    memset(f, 0, sizeof(*f));
    f->first = no;
    if ((rc = load_rec(v, no, v->rec2, &v->rec2_no))) return rc;
    e->attr = 0; e->time = 0; e->date = 1 << 5 | 1;
    if ((a = find_attr(v, v->rec2, AT_STD, 0)) && (val = res_value(a, &len)) && len >= 36) {
        dos_time(val + 8, v->tz, &e->time, &e->date);
        e->attr = (ofs_u16)(rd32(val + 32) & (OFS_NTFS_RDONLY | OFS_NTFS_HIDDEN | OFS_NTFS_SYSTEM | OFS_NTFS_ARCH));
    }
    if (rd16(v->rec2 + 0x16) & 2) {             /* a directory */
        e->attr |= OFS_NTFS_DIR;
        f->root = 1;
        return OFS_NTFS_OK;
    }
    if ((rc = data_attr(v, no, 0, &a))) return rc;
    if (!a[8]) {
        if (!res_value(a, &len)) return OFS_NTFS_EBAD;
        f->size = f->valid = len;
        f->nofat = 1;
        return OFS_NTFS_OK;
    }
    fl = rd16(a + 0x0C);
    /* encrypted, or compressed other than LZNT1 in units of 16 clusters
       of 4 KB at most: not read */
    if (fl & 0x4000 || (fl & 0xFF && ((fl & 0xFF) != 1 || a[0x22] != 4 || v->cshift > 3))) f->nofat = 2;
    else if (fl & 0xFF) f->nofat = 3;           /* compressed (LZNT1) */
    /* the size within the allocated size (holes of sparse files count) */
    if (rd32(a + 0x34) > rd32(a + 0x2C) || (rd32(a + 0x34) == rd32(a + 0x2C) && rd32(a + 0x30) > rd32(a + 0x28)))
        return OFS_NTFS_EBAD;
#if defined(__WATCOMC__) || defined(OFS_NARROW)
    if (rd32(a + 0x34)) { f->big = 1; f->size = 0xFFFFFFFFUL; }
    else f->size = rd32(a + 0x30);
    f->valid = rd32(a + 0x3C) ? 0xFFFFFFFFUL : rd32(a + 0x38);
#else
    f->size = (ofs_fsize)rd32(a + 0x34) << 32 | rd32(a + 0x30);
    f->valid = (ofs_fsize)rd32(a + 0x3C) << 32 | rd32(a + 0x38);
#endif
    if (f->valid > f->size) f->valid = f->size;
    return OFS_NTFS_OK;
}

/* ---- directories ---- */

#define IT_STAT 1                               /* attributes, size, time */
#define IT_DOS  2                               /* DOS-only names too */
#define UNUSED  2                               /* load_idx: a free index block */

/* the index block k of directory dir into v->idx; END past the last,
   1 when the block is not in use */
static int load_idx(struct ofs_ntfs *v, ofs_u32 dir, ofs_u32 k)
{
    const ofs_u8 *a, *bm, *val;
    ofs_u32 len, s, n = v->idx_size / 512, i, blocks, vcn;
    ofs_lba lba, lbas[OFS_NTFS_MAXREC / 512];
    int rc;
    if (v->idx_dir == dir && v->idx_no == k) return OFS_NTFS_OK;
    rc = ext_attr(v, dir, AT_IALLOC, 1, 0, v->rec, &v->rec_no, &a);
    if (rc == OFS_NTFS_END) return OFS_NTFS_END;
    if (rc) return rc;
    if (!a[8]) return OFS_NTFS_END;
    blocks = rd32(a + 0x34) ? NONE : rd32(a + 0x30) / v->idx_size;
    if (k >= blocks) return OFS_NTFS_END;
    rc = ext_attr(v, dir, AT_BITMAP, 1, (k / 4096) >> v->cshift, v->rec, &v->rec_no, &bm);
    if (!rc) {
        if (!bm[8]) {
            if (!(val = res_value(bm, &len))) return OFS_NTFS_EBAD;
            if (k / 8 < len && !(val[k / 8] >> (k & 7) & 1)) return UNUSED;
        } else {
            if ((rc = attr_sector(v, bm, k / 4096, &lba))) return rc;
            if (lba == NOBUF) return UNUSED;
            if (!(val = sector(v, lba))) return OFS_NTFS_EIO;
            if (!(val[k / 8 % 512] >> (k & 7) & 1)) return UNUSED;
        }
    } else if (rc != OFS_NTFS_END) return rc;
    if ((rc = ext_attr(v, dir, AT_IALLOC, 1, (k * n) >> v->cshift, v->rec, &v->rec_no, &a)))
        return rc == OFS_NTFS_END ? OFS_NTFS_EBAD : rc;
    /* the sectors of the block first (finding another extent may read a
       non-resident attribute list into idx), then the block */
    s = k * n;
    for (i = 0; i < n; ++i) {
        vcn = (s + i) >> v->cshift;
        if (!a[8] || vcn < rd32(a + 0x10) || vcn > rd32(a + 0x18))
            if ((rc = ext_attr(v, dir, AT_IALLOC, 1, vcn, v->rec, &v->rec_no, &a)))
                return rc == OFS_NTFS_END ? OFS_NTFS_EBAD : rc;
        if ((rc = attr_sector(v, a, s + i, &lbas[i]))) return rc;
        if (lbas[i] == NOBUF) return OFS_NTFS_EBAD;
    }
    v->idx_no = NONE;
    for (i = 0; i < n; ++i) {
        if (blk_read(v, lbas[i], v->idx + i * 512)) return OFS_NTFS_EIO;
        v->idx_lba[i] = lbas[i];
    }
    if (memcmp(v->idx, "INDX", 4) || fixup(v->idx, v->idx_size)) return OFS_NTFS_EBAD;
    v->idx_dir = dir; v->idx_no = k;
    return OFS_NTFS_OK;
}

static int iterate(struct ofs_ntfs *v, struct ofs_ntfs_file *dir, ofs_u32 *pos, struct ofs_ntfs_ent *e, unsigned how)
{
    const ofs_u8 *a, *base, *p, *key, *val;
    ofs_u32 blk, off, first, used, len, no;
    unsigned elen, klen, i;
    int rc;
    for (;;) {
        blk = *pos >> 16; off = *pos & 0xFFFF;
        if (!blk) {
            if ((rc = ext_attr(v, dir->first, AT_IROOT, 1, 0, v->rec, &v->rec_no, &a)))
                return rc == OFS_NTFS_END ? OFS_NTFS_EBAD : rc;
            if (!(val = res_value(a, &len)) || len < 32) return OFS_NTFS_EBAD;
            base = val + 16;
            if (rd32(base + 4) > len - 16) return OFS_NTFS_EBAD;
        } else {
            rc = load_idx(v, dir->first, blk - 1);
            if (rc == UNUSED) { *pos = (blk + 1) << 16; continue; }
            if (rc) return rc;
            base = v->idx + 24;
            if (rd32(base + 4) > (ofs_u32)v->idx_size - 24) return OFS_NTFS_EBAD;
        }
        first = rd32(base); used = rd32(base + 4);
        if (!off) off = first;
        if (off < first || off + 16 > used) return OFS_NTFS_EBAD;
        p = base + off;
        elen = rd16(p + 8); klen = rd16(p + 10);
        if (elen < 16 || off + elen > used || 16 + klen > elen) return OFS_NTFS_EBAD;
        if (rd16(p + 12) & 2) {                 /* the last entry of this node */
            *pos = (blk + 1) << 16;
            continue;
        }
        e->pos = blk << 16 | off;
        *pos = blk << 16 | (off + elen);
        no = rd32(p);
        key = p + 16;
        if (klen < 66 || 66u + 2u * key[64] > klen) return OFS_NTFS_EBAD;
        if (no < 16 || rd16(p + 4)) continue;   /* system files */
        e->ns = key[65];
        if (e->ns == 2 && !(how & IT_DOS)) continue;
        e->name_len = key[64];
        for (i = 0; i < e->name_len; ++i) e->name[i] = rd16(key + 66 + 2 * i);
        if (how & IT_STAT) {
            rc = stat_rec(v, no, e);
            if (rc == OFS_NTFS_END) continue;  /* a stale entry */
            if (rc) return rc;
        } else {
            memset(&e->file, 0, sizeof(e->file));
            e->file.first = no;
            e->attr = (rd32(key + 56) & F_DIR) ? OFS_NTFS_DIR : 0;
        }
        return OFS_NTFS_OK;
    }
}

int ofs_ntfs_dir_next(struct ofs_ntfs *v, struct ofs_ntfs_file *dir, ofs_u32 *pos, struct ofs_ntfs_ent *e)
{
    int rc = iterate(v, dir, pos, e, IT_STAT);
    return rc;
}

int ofs_ntfs_dir_name(struct ofs_ntfs *v, struct ofs_ntfs_file *dir, ofs_u32 *pos, struct ofs_ntfs_ent *e)
{
    return iterate(v, dir, pos, e, 0);
}

int ofs_ntfs_stat(struct ofs_ntfs *v, struct ofs_ntfs_ent *e)
{
    int rc = stat_rec(v, e->file.first, e);
    return rc == OFS_NTFS_END ? OFS_NTFS_EBAD : rc;
}

/* ---- 8.3 names ---- */

/* Calls fn for each $FILE_NAME of record no (also those in other records
   of its attribute list) until it returns 1; the value is in rec2. */
typedef int (*fname_fn)(struct ofs_ntfs *, const ofs_u8 *val, ofs_u32 len, void *ctx);

static int fnames_in(struct ofs_ntfs *v, fname_fn fn, void *ctx)
{
    const ofs_u8 *a = v->rec2 + rd16(v->rec2 + 0x14), *end = v->rec2 + v->rec_size, *val;
    ofs_u32 alen, len;
    for (; a + 0x18 <= end && rd32(a) != AT_END; a += alen) {
        alen = rd32(a + 4);
        if (alen < 0x18 || alen > (ofs_u32)(end - a)) return OFS_NTFS_EBAD;
        if (rd32(a) == AT_FNAME && (val = res_value(a, &len)) && len >= 0x42 &&
            0x42u + 2u * val[0x40] <= len && fn(v, val, len, ctx)) return 1;
    }
    return 0;
}

#define FN_EXT 8                                /* records of a list looked at */

static int each_fname(struct ofs_ntfs *v, ofs_u32 no, fname_fn fn, void *ctx)
{
    const ofs_u8 *l, *val, *end;
    ofs_u32 len, ext[FN_EXT], t;
    unsigned n = 0, i;
    int rc;
    if ((rc = load_rec(v, no, v->rec2, &v->rec2_no))) return rc;
    if ((rc = fnames_in(v, fn, ctx))) return rc;
    if (!(l = find_attr(v, v->rec2, AT_LIST, 0))) return 0;
    if (l[8]) {                                 /* non-resident: into idx */
        ofs_lba lba;
        if (rd32(l + 0x34) || (len = rd32(l + 0x30)) > OFS_NTFS_MAXREC) return OFS_NTFS_ESIZE;
        v->idx_no = NONE;
        for (i = 0; i * 512 < len; ++i) {
            if ((rc = attr_sector(v, l, i, &lba))) return rc;
            if (lba == NOBUF) return OFS_NTFS_EBAD;
            if (blk_read(v, lba, v->idx + i * 512)) return OFS_NTFS_EIO;
        }
        val = v->idx;
    } else if (!(val = res_value(l, &len))) return OFS_NTFS_EBAD;
    for (l = val, end = val + len; l + 0x1A <= end && rd16(l + 4) >= 0x1A && l + rd16(l + 4) <= end; l += rd16(l + 4)) {
        if (rd32(l) != AT_FNAME || (t = rd32(l + 0x10)) == no) continue;
        for (i = 0; i < n && ext[i] != t; ++i) ;
        if (i == n && n < FN_EXT) ext[n++] = t;
    }
    for (i = 0; i < n; ++i) {
        if ((rc = load_rec(v, ext[i], v->rec2, &v->rec2_no))) return rc == OFS_NTFS_END ? OFS_NTFS_EBAD : rc;
        if ((rc = fnames_in(v, fn, ctx))) return rc;
    }
    return 0;
}

/* the 8.3 name of a long name without a DOS name: as much of its basis as
   fits before "~" and the record number in base 36 (unique, and the
   record number leads ofs_ntfs_lookup straight to the file) */
static void gen_name(char fcb[11], ofs_u32 no)
{
    char tail[8];
    unsigned t = sizeof(tail), len, keep;
    do { tail[--t] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ"[no % 36]; no /= 36; } while (no && t > 1);
    tail[--t] = '~';
    for (len = 8; len && fcb[len-1] == ' '; --len) ;
    keep = 8 - (sizeof(tail) - t);
    if (len > keep) len = keep;
    memcpy(fcb + len, tail + t, sizeof(tail) - t);
    for (len += sizeof(tail) - t; len < 8; ++len) fcb[len] = ' ';
}

struct twin { ofs_u32 dir; char *fcb; };

/* a DOS name (name space 2) of the file in the directory */
static int find_twin(struct ofs_ntfs *v, const ofs_u8 *val, ofs_u32 len, void *ctx)
{
    struct twin *t = (struct twin *)ctx;
    ofs_u16 name[12];
    unsigned i, n = val[0x40];
    (void)len;
    if (val[0x41] != 2 || rd32(val) != t->dir || rd16(val + 4) || n > 12) return 0;
    for (i = 0; i < n; ++i) name[i] = rd16(val + 0x42 + 2 * i);
    ofs_cp_basis(v->cp, name, n, t->fcb);
    return 1;
}

int ofs_ntfs_short(struct ofs_ntfs *v, struct ofs_ntfs_file *d, const struct ofs_ntfs_ent *ent, char fcb[11])
{
    struct twin t;
    int rc;
    if (!ofs_cp_basis(v->cp, ent->name, ent->name_len, fcb)) return OFS_NTFS_OK;
    t.dir = d->first; t.fcb = fcb;              /* the 8.3 twin Windows made */
    if ((rc = each_fname(v, ent->file.first, find_twin, &t)) == 1) return OFS_NTFS_OK;
    if (rc && rc != OFS_NTFS_END) return rc;
    gen_name(fcb, ent->file.first);
    return OFS_NTFS_OK;
}

struct gen { ofs_u32 dir, no; const char *fcb; struct ofs_ntfs_ent *ent; int twin, found; };

/* a long name of the file in the directory whose made 8.3 name is fcb
   (into ent); a DOS name there means the file has no made name */
static int find_gen(struct ofs_ntfs *v, const ofs_u8 *val, ofs_u32 len, void *ctx)
{
    struct gen *g = (struct gen *)ctx;
    ofs_u16 name[255];
    char fcb[11];
    unsigned i, n = val[0x40];
    (void)len;
    if (rd32(val) != g->dir || rd16(val + 4)) return 0;
    if (val[0x41] == 2) { g->twin = 1; return 1; }
    for (i = 0; i < n; ++i) name[i] = rd16(val + 0x42 + 2 * i);
    if (g->found || !ofs_cp_basis(v->cp, name, n, fcb)) return 0;
    gen_name(fcb, g->no);
    if (memcmp(fcb, g->fcb, 11)) return 0;
    g->found = 1;
    g->ent->name_len = (ofs_u8)n;
    g->ent->ns = val[0x41];
    memcpy(g->ent->name, name, n * sizeof(name[0]));
    g->ent->file.first = g->no;
    return 0;
}

/* a long name of the file in the directory (for a DOS name found) */
static int find_long(struct ofs_ntfs *v, const ofs_u8 *val, ofs_u32 len, void *ctx)
{
    struct gen *g = (struct gen *)ctx;
    unsigned i;
    (void)v; (void)len;
    if (rd32(val) != g->dir || rd16(val + 4) || val[0x41] == 2) return 0;
    g->ent->name_len = val[0x40];
    g->ent->ns = val[0x41];
    for (i = 0; i < g->ent->name_len; ++i) g->ent->name[i] = rd16(val + 0x42 + 2 * i);
    return 1;
}

/* the record number of a made 8.3 name ("~" and base 36), or NONE */
static ofs_u32 gen_no(const char fcb[11])
{
    ofs_u32 no = 0;
    unsigned i = 8, digits = 0;
    while (i && fcb[i-1] == ' ') --i;
    while (i && fcb[i-1] != '~') {
        --i;
        if (!(fcb[i] >= '0' && fcb[i] <= '9') && !(fcb[i] >= 'A' && fcb[i] <= 'Z')) return NONE;
        ++digits;
    }
    if (!i || !digits || digits > 6) return NONE;
    for (; i < 8 && fcb[i] != ' '; ++i)
        no = no * 36 + (ofs_u32)(fcb[i] <= '9' ? fcb[i] - '0' : fcb[i] - 'A' + 10);
    return no;
}

int ofs_ntfs_lookup(struct ofs_ntfs *v, struct ofs_ntfs_file *d, const char fcb[11], struct ofs_ntfs_ent *ent)
{
    char name[11];
    ofs_u32 pos = 0, no;
    struct gen g;
    int r;
    g.dir = d->first; g.fcb = fcb; g.ent = ent; g.twin = g.found = 0;
    /* a made name: its record, no search */
    if ((no = gen_no(fcb)) != NONE && no >= 16) {
        g.no = no;
        r = each_fname(v, no, find_gen, &g);
        if (r == OFS_NTFS_EIO) return r;
        if (r == 0 && g.found) goto found;
        /* else a real 8.3 name with "~", or no such record */
        g.found = g.twin = 0;
    }
    /* else the 8.3 names as they are, and the DOS names Windows made */
    for (;;) {
        r = iterate(v, d, &pos, ent, IT_DOS);
        if (r) return r;
        if (ofs_cp_basis(v->cp, ent->name, ent->name_len, name) || memcmp(name, fcb, 11)) continue;
        if (ent->ns == 2) {                     /* its long name */
            g.no = ent->file.first;
            if ((r = each_fname(v, g.no, find_long, &g)) < 0) return r == OFS_NTFS_END ? OFS_NTFS_EBAD : r;
            if (!r) return OFS_NTFS_EBAD;
            ent->file.first = g.no;
        }
        break;
    }
found:
    no = ent->file.first;
    r = stat_rec(v, no, ent);
    ent->pos = 0;
    return r == OFS_NTFS_END ? OFS_NTFS_EBAD : r;
}

int ofs_ntfs_free(struct ofs_ntfs *v, ofs_u32 *free_clusters)
{
    const ofs_u8 *a, *s;
    ofs_u32 c = 0, sec, nsec = (v->clusters + 4095) / 4096, bit, left;
    ofs_lba lba;
    unsigned i, b;
    int rc;
    if (v->free != NONE) { *free_clusters = v->free; return OFS_NTFS_OK; }
    if ((rc = load_rec(v, 6, v->rec2, &v->rec2_no))) return rc == OFS_NTFS_END ? OFS_NTFS_EBAD : rc;
    if (!(a = find_attr(v, v->rec2, AT_DATA, 0)) || !a[8]) return OFS_NTFS_EBAD;
    for (sec = 0; sec < nsec; ++sec) {
        if ((rc = attr_sector(v, a, sec, &lba))) return rc;
        left = v->clusters - sec * 4096;
        if (lba == NOBUF) { c += left > 4096 ? 4096 : left; continue; }
        if (!(s = sector(v, lba))) return OFS_NTFS_EIO;
        for (i = 0; i < 512 && left; ++i)
            for (b = 0, bit = s[i]; b < 8 && left; ++b, --left)
                if (!(bit >> b & 1)) ++c;
    }
    v->free = c;
    *free_clusters = c;
    return OFS_NTFS_OK;
}

#ifndef OFS_READONLY
/* ---- writing, stage 1: overwriting in place ----
   Data that has its clusters (or its place in the MFT record) is written
   where it is; nothing is allocated, no size changes, so the volume's
   metadata stays as it was but for the file's $STANDARD_INFORMATION
   (its times and the archive bit) and a resident value.  Those MFT
   records are written with a new update sequence number.  $LogFile is
   not written: the volume is only written when Windows left it clean
   (ofs_ntfs_set_write), so it has nothing to replay over our changes. */

static void wr16(ofs_u8 *p, ofs_u16 x) { p[0] = (ofs_u8)x; p[1] = (ofs_u8)(x >> 8); }
static void wr32(ofs_u8 *p, ofs_u32 x) { wr16(p, (ofs_u16)x); wr16(p + 2, (ofs_u16)(x >> 16)); }
static ofs_u64 rd64(const ofs_u8 *p);
static int runs_get(const ofs_u8 *a, struct ofs_ntfs_run *rs, unsigned *n);

/* the device sector in buf back to the device */
static int buf_flush(struct ofs_ntfs *v)
{
    if (v->write(v->dev, v->start + v->buf_lba, v->buf)) { v->buf_lba = NOBUF; return OFS_NTFS_EWRITE; }
    return OFS_NTFS_OK;
}

/* n bytes from src at offset in of block lba */
static int blk_patch(struct ofs_ntfs *v, ofs_lba lba, unsigned in, const ofs_u8 OFS_FAR *src, unsigned n)
{
    ofs_u8 *s;
    if (v->sshift == 9 && !in && n == 512) {    /* a whole sector */
        if (v->buf_lba == lba) v->buf_lba = NOBUF;
        return v->write(v->dev, v->start + lba, src) ? OFS_NTFS_EWRITE : OFS_NTFS_OK;
    }
    if (!(s = (ofs_u8 *)sector(v, lba))) return OFS_NTFS_EIO;
    fmemcpy(s + in, src, n);
    return buf_flush(v);
}

/* the update sequence of a record or index block of size bytes in memory
   (as fixup left it) for writing: the next number at the end of each
   block, their words into the array; usa_open undoes it */
static int usa_seal(ofs_u8 *b, unsigned size)
{
    unsigned ofs = rd16(b + 4), cnt = rd16(b + 6), i;
    ofs_u16 usn = (ofs_u16)(rd16(b + ofs) + 1);
    if (cnt != size / 512 + 1 || ofs + 2 * cnt > 512) return OFS_NTFS_EBAD;
    if (!usn || usn == 0xFFFF) usn = 1;
    wr16(b + ofs, usn);
    for (i = 1; i < cnt; ++i) {
        b[ofs + 2 * i] = b[i * 512 - 2];
        b[ofs + 2 * i + 1] = b[i * 512 - 1];
        wr16(b + i * 512 - 2, usn);
    }
    return OFS_NTFS_OK;
}

static void usa_open(ofs_u8 *b)
{
    unsigned ofs = rd16(b + 4), cnt = rd16(b + 6), i;
    for (i = 1; i < cnt; ++i) {
        b[i * 512 - 2] = b[ofs + 2 * i];
        b[i * 512 - 1] = b[ofs + 2 * i + 1];
    }
}

/* MFT record no from b (as load_rec left it, fixups undone), with the
   next update sequence number; b stays as it was */
static int write_rec(struct ofs_ntfs *v, ofs_u32 no, ofs_u8 *b)
{
    unsigned n = v->rec_size / 512, i;
    ofs_lba lba;
    int rc;
    /* the other cache must not keep the record as it was */
    if (b != v->rec && v->rec_no == no) v->rec_no = NONE;
    if (b != v->rec2 && v->rec2_no == no) v->rec2_no = NONE;
    if ((rc = usa_seal(b, v->rec_size))) return rc;
    for (i = 0; i < n && !rc; ++i)
        if (!(rc = mft_sector(v, no * n + i, &lba))) rc = blk_patch(v, lba, 0, b + i * 512, 512);
    for (i = 0; i < n && !rc && no < v->mirr_n; ++i)   /* $MFTMirr's copy */
        rc = blk_patch(v, ((ofs_lba)v->mirr_lcn << v->cshift) + (ofs_lba)no * n + i, 0, b + i * 512, 512);
    usa_open(b);
    return rc;
}

/* the index block in idx (load_idx) back where it came from */
static int write_idx(struct ofs_ntfs *v)
{
    unsigned n = v->idx_size / 512, i;
    int rc;
    if ((rc = usa_seal(v->idx, v->idx_size))) return rc;
    for (i = 0; i < n && !rc; ++i) rc = blk_patch(v, v->idx_lba[i], 0, v->idx + i * 512, 512);
    usa_open(v->idx);
    return rc;
}

/* DOS local time -> NTFS time (100 ns from 1601, UTC) */
static ofs_u64 nt_time(ofs_u16 time, ofs_u16 date, ofs_s16 tz)
{
    ofs_u32 y = 1980 + (date >> 9), m = date >> 5 & 15, d = date & 31, era, yoe, doy, doe, days;
    ofs_u64 t;
    if (m < 1 || m > 12) m = 1;
    if (!d) d = 1;
    /* days from civil (Howard Hinnant), from 1970-01-01 */
    y -= m <= 2;
    era = y / 400; yoe = y - era * 400;
    doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
    doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    days = era * 146097 + doe - 719468UL;
    t = (ofs_u64)days * 86400 + (ofs_u32)(time >> 11) * 3600 + (time >> 5 & 63) * 60 + (time & 31) * 2;
    if (tz < 0) t += (ofs_u32)-tz * 60UL; else t -= (ofs_u32)tz * 60UL;   /* to UTC */
    return (t + 11644473600UL) * 10000000UL;
}

/* $LogFile's restart pages (through idx): the later one (by its LSN) must
   say the volume is clean -- no client in use, or the clean flag (as
   ntfs-3g's ntfs_check_logfile takes it).  An empty $LogFile (all 0xFF,
   as mkntfs and ntfs-3g leave it; Windows sets it up anew) is clean. */
static int log_clean(struct ofs_ntfs *v)
{
    struct ofs_ntfs_ent e;
    ofs_u32 at = 0, psize = 4096, p, hi, lo, best_hi = 0, best_lo = 0;
    unsigned pg, ra, clean = 0, any = 0, empty = 0, i;
    ofs_u16 done;
    int rc;
    if ((rc = stat_rec(v, 2, &e))) return rc == OFS_NTFS_END ? OFS_NTFS_EBAD : rc;
    if (e.file.nofat) return OFS_NTFS_EROFS;
    for (pg = 0; pg < 2; ++pg, at = psize) {   /* the second at the first's page size */
        v->idx_no = NONE;
        if ((rc = ofs_ntfs_read(v, &e.file, at, v->idx, 512, &done))) return rc;
        if (done < 512) return OFS_NTFS_EROFS;
        if (!memcmp(v->idx, "CHKD", 4)) return OFS_NTFS_EROFS;  /* chkdsk left it */
        if (memcmp(v->idx, "RSTR", 4)) {
            for (i = 0; i < 512 && v->idx[i] == 0xFF; ++i) ;
            empty += i == 512;
            continue;
        }
        p = rd32(v->idx + 0x10);               /* the system page size */
        if (p < 512 || p > OFS_NTFS_MAXREC || (p & (p - 1))) return OFS_NTFS_EROFS;
        if (!pg) psize = p;
        if ((rc = ofs_ntfs_read(v, &e.file, at, v->idx, (ofs_u16)p, &done))) return rc;
        if (done < p || fixup(v->idx, (unsigned)p)) continue;
        ra = rd16(v->idx + 0x18);
        if (ra + 0x10 > p) continue;
        lo = rd32(v->idx + ra); hi = rd32(v->idx + ra + 4);
        if (any && (hi < best_hi || (hi == best_hi && lo <= best_lo))) continue;
        any = 1; best_hi = hi; best_lo = lo;
        clean = rd16(v->idx + ra + 0x0C) == 0xFFFF || (rd16(v->idx + ra + 0x0E) & 2);
    }
    v->idx_no = NONE;
    if (!any) return empty == 2 ? OFS_NTFS_OK : OFS_NTFS_EROFS;
    return clean ? OFS_NTFS_OK : OFS_NTFS_EROFS;
}

static int create_setup(struct ofs_ntfs *v);

int ofs_ntfs_set_write(struct ofs_ntfs *v, ofs_ntfs_write_fn write)
{
    static const char hiber[11] = { 'H','I','B','E','R','F','I','L','S','Y','S' };
    struct ofs_ntfs_ent e;
    struct ofs_ntfs_file root;
    const ofs_u8 *a, *val;
    ofs_u32 len;
    ofs_u16 done;
    int rc;
    v->write = 0;
    if (!write || v->dirty) return OFS_NTFS_EROFS;
    /* NTFS 3.0 and 3.1 only (Windows 2000 and later) */
    if ((rc = load_rec(v, 3, v->rec, &v->rec_no))) return rc == OFS_NTFS_END ? OFS_NTFS_EBAD : rc;
    if (!(a = find_attr(v, v->rec, AT_VINFO, 0)) || !(val = res_value(a, &len)) || len < 12 ||
        val[8] != 3 || val[9] > 1)
        return OFS_NTFS_EROFS;
    v->ver_minor = val[9];
    if ((rc = log_clean(v))) return rc;
    /* hibernated Windows (also its fast startup) keeps the volume open */
    ofs_ntfs_rootdir(v, &root);
    rc = ofs_ntfs_lookup(v, &root, hiber, &e);
    if (!rc && e.file.size >= 4) {
        ofs_u8 h[4];
        if ((rc = ofs_ntfs_read(v, &e.file, 0, h, 4, &done))) return rc;
        if (done == 4 && (!memcmp(h, "hibr", 4) || !memcmp(h, "HIBR", 4))) return OFS_NTFS_EROFS;
    } else if (rc != OFS_NTFS_END && rc != OFS_NTFS_OK) {
        return rc;
    }
    /* the runs of $Bitmap, for allocating (more than four: no allocation) */
    v->bm_nruns = 0;
    if ((rc = load_rec(v, 6, v->rec2, &v->rec2_no))) return rc == OFS_NTFS_END ? OFS_NTFS_EBAD : rc;
    if ((a = find_attr(v, v->rec2, AT_DATA, 0)) && a[8] && !find_attr(v, v->rec2, AT_LIST, 0)) {
        struct ofs_ntfs_run *rs = (struct ofs_ntfs_run *)v->idx;
        unsigned n, i;
        v->idx_no = NONE;
        if (!runs_get(a, rs, &n) && n <= 4 && rd64(a + 0x30) * 8 >= v->clusters) {
            for (i = 0; i < n && rs[i].lcn != NONE; ++i) v->bmr[i] = rs[i];
            if (i == n) v->bm_nruns = (ofs_u8)n;
        }
    }
    v->write = write;
    if ((rc = create_setup(v))) { v->write = 0; return rc; }
    return OFS_NTFS_OK;
}

/* ---- writing, stage 2a: sizes ----
   A file's $DATA in its base record (also with an attribute list, as
   long as $DATA is one attribute there) grows and shrinks: clusters from
   $Bitmap, the runlist written anew in the record (OFS_NTFS_ENOSPACE when
   it does not fit), resident data moved out of the record when it no
   longer fits.  The first change of metadata empties $LogFile (all 0xFF,
   as ntfs-3g does): Windows sets it up anew and replays nothing over it. */

#define MAXRUNS (OFS_NTFS_MAXREC / sizeof(struct ofs_ntfs_run))   /* in idx */

static void wr64(ofs_u8 *p, ofs_u64 x) { wr32(p, (ofs_u32)x); wr32(p + 4, (ofs_u32)(x >> 32)); }
static ofs_u64 rd64(const ofs_u8 *p) { return (ofs_u64)rd32(p + 4) << 32 | rd32(p); }
static ofs_u32 clu_bytes(const struct ofs_ntfs *v) { return 512UL << v->cshift; }

/* the block of $Bitmap with cluster c */
static int bm_sector(struct ofs_ntfs *v, ofs_u32 c, ofs_u8 **s)
{
    ofs_u32 blk = c >> 12, vcn = blk >> v->cshift;
    unsigned i;
    for (i = 0; i < v->bm_nruns; ++i)
        if (vcn >= v->bmr[i].vcn && vcn - v->bmr[i].vcn < v->bmr[i].len) {
            ofs_lba lba = ((ofs_lba)(v->bmr[i].lcn + (vcn - v->bmr[i].vcn)) << v->cshift) +
                          (blk & ((1UL << v->cshift) - 1));
            return (*s = (ofs_u8 *)sector(v, lba)) ? OFS_NTFS_OK : OFS_NTFS_EIO;
        }
    return OFS_NTFS_EBAD;
}

/* n clusters from c in $Bitmap: in use (on) or free */
static int bm_set(struct ofs_ntfs *v, ofs_u32 c, ofs_u32 n, int on)
{
    ofs_u32 blk;
    ofs_u8 *s, m;
    int rc;
    if (c >= v->clusters || n > v->clusters - c) return OFS_NTFS_EBAD;
    while (n) {
        blk = c >> 12;
        if ((rc = bm_sector(v, c, &s))) return rc;
        for (; n && c >> 12 == blk; ++c, --n) {
            m = (ofs_u8)(1u << (c & 7));
            if (!(s[c >> 3 & 511] & m) != !on) {       /* a change */
                if (v->free != NONE) v->free += on ? (ofs_u32)-1 : 1;
                s[c >> 3 & 511] ^= m;
            }
        }
        if ((rc = buf_flush(v))) return rc;
    }
    return OFS_NTFS_OK;
}

#define MINRUN 16                       /* a new extent is looked for this long at least */

/* free clusters for want more of a file whose clusters end at hint: those
   right after it when free (the file goes on in one piece); else, round
   the volume once from hint, the first run of max(want, MINRUN) (so that
   the file can go on after it again), else the longest run there is (runs
   do not wrap).  *lcn, *len (at most want); OFS_NTFS_EFULL: none */
static int bm_find(struct ofs_ntfs *v, ofs_u32 hint, ofs_u32 want, ofs_u32 *lcn, ofs_u32 *len)
{
    ofs_u32 c = hint < v->clusters ? hint : 0, seen, start = 0, run = 0, enough = want > MINRUN ? want : MINRUN;
    ofs_u8 *s;
    int rc, used, first = 1;
    *lcn = 0; *len = 0;
    for (seen = 0; seen <= v->clusters; ++seen) {
        used = 1;
        if (seen < v->clusters) {
            if ((rc = bm_sector(v, c, &s))) return rc;
            used = s[c >> 3 & 511] >> (c & 7) & 1;
        }
        if (!used) {
            if (!run++) start = c;
            if (run == enough || (first && run == want)) { *lcn = start; *len = want; return OFS_NTFS_OK; }
        }
        if ((used || c + 1 == v->clusters) && run) {  /* a run ends */
            if (first) { *lcn = start; *len = run; return OFS_NTFS_OK; }   /* right after the file */
            if (run > *len) { *lcn = start; *len = run; }
            run = 0;
        }
        first = first && !used;
        if (++c == v->clusters) c = 0;
    }
    if (*len > want) *len = want;
    return *len ? OFS_NTFS_OK : OFS_NTFS_EFULL;
}

/* the runs of the non-resident attribute a into rs (*n of them) */
static int runs_get(const ofs_u8 *a, struct ofs_ntfs_run *rs, unsigned *n)
{
    const ofs_u8 *rl = a + rd16(a + 0x20), *end = a + rd32(a + 4);
    ofs_u32 cur = rd32(a + 0x10), lcn = 0, len;
    ofs_s32 delta;
    unsigned ln, on, i;
    *n = 0;
    if (rd16(a + 0x20) > rd32(a + 4)) return OFS_NTFS_EBAD;
    while (rl < end && *rl) {
        ln = *rl & 15; on = *rl >> 4;
        if (!ln || ln > 8 || on > 8 || rl + 1 + ln + on > end) return OFS_NTFS_EBAD;
        ++rl;
        for (len = 0, i = 0; i < ln; ++i) if (i < 4) len |= (ofs_u32)rl[i] << (8 * i);
        rl += ln;
        if (*n == MAXRUNS) return OFS_NTFS_ENOSPACE;
        rs[*n].vcn = cur; rs[*n].len = len; rs[*n].lcn = NONE;
        if (on) {
            for (delta = 0, i = 0; i < on && i < 4; ++i) delta |= (ofs_s32)((ofs_u32)rl[i] << (8 * i));
            if (on < 4 && (rl[on - 1] & 0x80)) delta |= (ofs_s32)(NONE << (8 * on));
            rl += on;
            lcn += (ofs_u32)delta;
            rs[*n].lcn = lcn;
        }
        cur += len;
        ++*n;
    }
    return OFS_NTFS_OK;
}

/* bytes of a signed number, as the mapping pairs store it */
static unsigned sbytes(ofs_s64 x)
{
    unsigned n = 1;
    while (n < 8 && (x < -((ofs_s64)1 << (8 * n - 1)) || x >= (ofs_s64)1 << (8 * n - 1))) ++n;
    return n;
}

/* the mapping pairs of rs into out (out 0: only counted); their bytes
   with the closing 0 */
static unsigned runs_put(const struct ofs_ntfs_run *rs, unsigned n, ofs_u8 *out)
{
    unsigned i, p = 0, ln, on, k;
    ofs_s64 prev = 0, d = 0;
    for (i = 0; i < n; ++i) {
        ln = sbytes((ofs_s64)rs[i].len);
        on = 0;
        if (rs[i].lcn != NONE) { d = (ofs_s64)rs[i].lcn - prev; on = sbytes(d); prev = rs[i].lcn; }
        if (out) {
            out[p] = (ofs_u8)(on << 4 | ln);
            for (k = 0; k < ln; ++k) out[p + 1 + k] = (ofs_u8)((ofs_u64)rs[i].len >> (8 * k));
            for (k = 0; k < on; ++k) out[p + 1 + ln + k] = (ofs_u8)((ofs_u64)d >> (8 * k));
        }
        p += 1 + ln + on;
    }
    if (out) out[p] = 0;
    return p + 1;
}

/* attribute a of record b becomes nlen bytes long (a multiple of 8): what
   follows it moves; OFS_NTFS_ENOSPACE when the record has no room */
static int attr_resize(struct ofs_ntfs *v, ofs_u8 *b, ofs_u8 *a, ofs_u32 nlen)
{
    ofs_u32 olen = rd32(a + 4), used = rd32(b + 0x18), alloc = rd32(b + 0x1C), at = (ofs_u32)(a - b);
    if (alloc > v->rec_size || used > alloc || at + olen > used) return OFS_NTFS_EBAD;
    if (used - olen + nlen > alloc) return OFS_NTFS_ENOSPACE;
    memmove(a + nlen, a + olen, used - at - olen);
    if (nlen > olen) memset(a + olen, 0, nlen - olen);
    wr32(a + 4, nlen);
    wr32(b + 0x18, used - olen + nlen);
    return OFS_NTFS_OK;
}

/* the runs of the non-resident attribute a (lowest VCN 0) and its sizes */
static int put_runs(struct ofs_ntfs *v, ofs_u8 *b, ofs_u8 *a, const struct ofs_ntfs_run *rs, unsigned n,
                    ofs_u64 size, ofs_u64 init)
{
    ofs_u32 mpoff = rd16(a + 0x20), bytes = runs_put(rs, n, 0), total = 0, real = 0;
    unsigned i;
    int rc;
    for (i = 0; i < n; ++i) { total += rs[i].len; if (rs[i].lcn != NONE) real += rs[i].len; }
    if ((rc = attr_resize(v, b, a, (mpoff + bytes + 7) & ~7UL))) return rc;
    runs_put(rs, n, a + mpoff);
    wr64(a + 0x10, 0);
    wr64(a + 0x18, total ? (ofs_u64)total - 1 : ~(ofs_u64)0);
    wr64(a + 0x28, (ofs_u64)total * clu_bytes(v));
    wr64(a + 0x30, size);
    wr64(a + 0x38, init);
    if (mpoff >= 0x48 && rd16(a + 0x0C) & 0x8000)  /* sparse: the clusters it has */
        wr64(a + 0x40, (ofs_u64)real * clu_bytes(v));
    return OFS_NTFS_OK;
}

/* $LogFile emptied (once a mount), before the first change of metadata */
static int log_reset(struct ofs_ntfs *v)
{
    static ofs_u8 fill[512];
    struct ofs_ntfs_ent e;
    struct ofs_ntfs_run r;
    const ofs_u8 *a;
    ofs_u32 blk, nblk, vcn;
    ofs_lba lba;
    int rc;
    if (v->log_reset) return OFS_NTFS_OK;
    memset(fill, 0xFF, sizeof(fill));
    if ((rc = stat_rec(v, 2, &e))) return rc == OFS_NTFS_END ? OFS_NTFS_EBAD : rc;
    if (e.file.nofat) return OFS_NTFS_EBAD;
    nblk = (ofs_u32)((e.file.size + 511) >> 9);
    for (blk = 0; blk < nblk; ++blk) {
        vcn = blk >> v->cshift;
        if ((rc = data_attr(v, 2, vcn, &a))) return rc;
        if (!a[8] || (rc = attr_run(a, vcn, &r))) return rc ? rc : OFS_NTFS_EBAD;
        if (r.lcn == NONE) continue;
        lba = ((ofs_lba)(r.lcn + (vcn - r.vcn)) << v->cshift) + (blk & ((1UL << v->cshift) - 1));
        if ((rc = blk_patch(v, lba, 0, fill, 512))) return rc;
    }
    v->log_reset = 1;
    return OFS_NTFS_OK;
}

/* The entries of record no in the index of directory dir (root and
   blocks): their duplicated information -- the times and attributes of
   si (a $STANDARD_INFORMATION value), the sizes -- as Windows keeps it
   there for DIR.  Every entry: the Win32 and the DOS name. */
static int dup_info(struct ofs_ntfs *v, ofs_u32 dir, ofs_u32 no, const ofs_u8 *si, ofs_u64 alloc, ofs_u64 size)
{
    const ofs_u8 *a, *val;
    ofs_u8 *base, *p, *key;
    ofs_u32 len, first, used, off, elen, k;
    int rc, changed, blk;
    for (blk = -1;; ++blk) {
        if (blk < 0) {
            if ((rc = ext_attr(v, dir, AT_IROOT, 1, 0, v->rec, &v->rec_no, &a)))
                return rc == OFS_NTFS_END ? OFS_NTFS_EBAD : rc;
            if (!(val = res_value(a, &len)) || len < 32) return OFS_NTFS_EBAD;
            base = (ofs_u8 *)val + 16;
            if (rd32(base + 4) > len - 16) return OFS_NTFS_EBAD;
        } else {
            rc = load_idx(v, dir, (ofs_u32)blk);
            if (rc == OFS_NTFS_END) return OFS_NTFS_OK;
            if (rc == UNUSED) continue;
            if (rc) return rc;
            base = v->idx + 24;
            if (rd32(base + 4) > (ofs_u32)v->idx_size - 24) return OFS_NTFS_EBAD;
        }
        first = rd32(base); used = rd32(base + 4);
        changed = 0;
        for (off = first; off + 16 <= used; off += elen) {
            p = base + off;
            elen = rd16(p + 8);
            if (elen < 16 || off + elen > used) return OFS_NTFS_EBAD;
            if (rd16(p + 12) & 2) break;        /* the last entry */
            if (rd32(p) != no || rd16(p + 4) || rd16(p + 10) < 0x42 || 16u + rd16(p + 10) > elen) continue;
            key = p + 16;
            for (k = 0; k < 32; ++k) key[8 + k] = si[k];   /* created, written, changed, read */
            wr64(key + 0x28, alloc);
            wr64(key + 0x30, size);
            wr32(key + 0x38, (rd32(key + 0x38) & F_DIR) | (rd32(si + 0x20) & ~F_DIR));
            changed = 1;
        }
        if (changed && (rc = blk < 0 ? write_rec(v, v->rec_no, v->rec) : write_idx(v))) return rc;
    }
}

/* what ofs_ntfs_set_time/_attr change in $STANDARD_INFORMATION */
struct si_change {
    int at;                      /* a time's offset there, -1: none */
    ofs_u16 time, date;
    ofs_u8 cs10;
    int attr;                    /* the attributes (RDONLY, HIDDEN, SYSTEM, ARCH), -1: as they are */
};

static int finish_si(struct ofs_ntfs *v, struct ofs_ntfs_file *f, ofs_u16 time, ofs_u16 date, int sizes,
                     const struct si_change *c);

/* $STANDARD_INFORMATION (the time written, the archive bit) and, with
   sizes, the sizes of the $FILE_NAMEs of the base record no (in rec2), the
   record written; then the entries in the directories (dup_info) */
static int finish(struct ofs_ntfs *v, struct ofs_ntfs_file *f, ofs_u16 time, ofs_u16 date, int sizes)
{
    return finish_si(v, f, time, date, sizes, 0);
}

/* the same, or with c the change asked for instead of the write's */
static int finish_si(struct ofs_ntfs *v, struct ofs_ntfs_file *f, ofs_u16 time, ofs_u16 date, int sizes,
                     const struct si_change *c)
{
    ofs_u8 *val, *b, si[0x24];
    const ofs_u8 *a, *d;
    ofs_u32 len, alen, parent[4];
    ofs_u64 t, alloc = 0;
    unsigned np = 0, i;
    int rc;
    if ((rc = load_rec(v, f->first, v->rec2, &v->rec2_no))) return rc == OFS_NTFS_END ? OFS_NTFS_EBAD : rc;
    b = v->rec2;
    if (!(a = find_attr(v, b, AT_STD, 0)) || !(val = (ofs_u8 *)res_value(a, &len)) || len < 36)
        return OFS_NTFS_EBAD;
    if (c) {
        if (c->at >= 0) wr64(val + c->at, nt_time(c->time, c->date, v->tz) + (ofs_u64)c->cs10 * 100000UL);
        if (c->attr >= 0)
            wr32(val + 0x20, (rd32(val + 0x20) & ~(ofs_u32)0x27) | ((ofs_u32)c->attr & 0x27));
    } else {
        if (date) {
            t = nt_time(time, date, v->tz);
            wr64(val + 0x08, t);                /* written */
            wr64(val + 0x10, t);                /* record changed */
        }
        if (!(rd16(b + 0x16) & 2)) wr32(val + 0x20, rd32(val + 0x20) | OFS_NTFS_ARCH);   /* files only */
    }
    memcpy(si, val, 0x24);
    if ((d = find_attr(v, b, AT_DATA, 0)))
        alloc = d[8] ? rd64(d + 0x28) : (rd32(d + 0x10) + 7) & ~7UL;
    for (a = b + rd16(b + 0x14); a + 0x18 <= b + v->rec_size && rd32(a) != AT_END; a += alen) {
        alen = rd32(a + 4);
        if (alen < 0x18 || alen > (ofs_u32)(b + v->rec_size - a)) return OFS_NTFS_EBAD;
        if (rd32(a) == AT_FNAME && (val = (ofs_u8 *)res_value(a, &len)) && len >= 0x42) {
            if (sizes) {
                wr64(val + 0x28, alloc);
                wr64(val + 0x30, (ofs_u64)f->size);
            }
            if (!rd16(val + 4) && np < 4 && (!np || parent[np - 1] != rd32(val))) parent[np++] = rd32(val);
        }
    }
    if ((rc = write_rec(v, f->first, b))) return rc;
    for (i = 0; i < np; ++i)
        if ((rc = dup_info(v, parent[i], f->first, si, alloc, (ofs_u64)f->size))) return rc;
    return OFS_NTFS_OK;
}

/* the unnamed $DATA of the base record of f, as one attribute there, in
   rec2: *pa */
static int base_data(struct ofs_ntfs *v, struct ofs_ntfs_file *f, ofs_u8 **pa)
{
    const ofs_u8 *a;
    int rc;
    if ((rc = data_attr(v, f->first, 0, &a))) return rc;
    if (v->rec2_no != f->first) return OFS_NTFS_ENOSPACE;   /* in an extension record */
    if (a[8] && (rd32(a + 0x10) || rd32(a + 0x14) ||
                 rd64(a + 0x18) + 1 != (rd64(a + 0x28) >> (9 + v->cshift))))
        return OFS_NTFS_ENOSPACE;                          /* more extents */
    *pa = (ofs_u8 *)a;
    return OFS_NTFS_OK;
}

/* clusters for runs rs[*n..] until they cover need clusters, from hint on */
static int grow_runs(struct ofs_ntfs *v, struct ofs_ntfs_run *rs, unsigned *n, ofs_u32 have, ofs_u32 need)
{
    ofs_u32 lcn, len, hint = v->bm_hint;
    unsigned i;
    int rc;
    for (i = *n; i--;)                          /* after the last cluster it has */
        if (rs[i].lcn != NONE) { hint = rs[i].lcn + rs[i].len; break; }
    while (have < need) {
        if ((rc = bm_find(v, hint, need - have, &lcn, &len))) return rc;
        if ((rc = bm_set(v, lcn, len, 1))) return rc;
        if (*n && rs[*n - 1].lcn != NONE && rs[*n - 1].lcn + rs[*n - 1].len == lcn) {
            rs[*n - 1].len += len;
        } else {
            if (*n == MAXRUNS) { bm_set(v, lcn, len, 0); return OFS_NTFS_ENOSPACE; }
            rs[*n].vcn = have; rs[*n].len = len; rs[*n].lcn = lcn;
            ++*n;
        }
        have += len;
        hint = v->bm_hint = lcn + len;
    }
    return OFS_NTFS_OK;
}

/* the clusters of rs from cluster keep on freed, rs cut there */
static int cut_runs(struct ofs_ntfs *v, struct ofs_ntfs_run *rs, unsigned *n, ofs_u32 keep)
{
    unsigned i;
    ofs_u32 off;
    int rc;
    for (i = 0; i < *n; ++i) {
        if (rs[i].vcn + rs[i].len <= keep) continue;
        off = keep > rs[i].vcn ? keep - rs[i].vcn : 0;
        if (rs[i].lcn != NONE && (rc = bm_set(v, rs[i].lcn + off, rs[i].len - off, 0))) return rc;
        rs[i].len = off;
    }
    while (*n && !rs[*n - 1].len) --*n;
    return OFS_NTFS_OK;
}

int ofs_ntfs_set_time(struct ofs_ntfs *v, struct ofs_ntfs_file *f, unsigned which, ofs_u16 time, ofs_u16 date,
                      ofs_u8 cs10)
{
    struct si_change c;
    if (!v->write) return OFS_NTFS_EROFS;
    if (which != OFS_NTFS_CREATED && which != OFS_NTFS_WRITTEN && which != OFS_NTFS_READ) return OFS_NTFS_EBAD;
    c.at = (int)which; c.time = time; c.date = date; c.cs10 = cs10; c.attr = -1;
    return finish_si(v, f, 0, 0, 0, &c);
}

int ofs_ntfs_set_attr(struct ofs_ntfs *v, struct ofs_ntfs_file *f, ofs_u8 attr)
{
    struct si_change c;
    if (!v->write) return OFS_NTFS_EROFS;
    c.at = -1; c.time = c.date = 0; c.cs10 = 0; c.attr = attr & 0x27;
    return finish_si(v, f, 0, 0, 0, &c);
}

int ofs_ntfs_resize(struct ofs_ntfs *v, struct ofs_ntfs_file *f, ofs_fsize size, ofs_u16 time, ofs_u16 date)
{
    struct ofs_ntfs_run *rs = (struct ofs_ntfs_run *)v->idx;
    ofs_u32 cb = clu_bytes(v), have, need, vlen, voff, i;
    ofs_u64 init;
    unsigned n = 0;
    ofs_u8 *a, *val, blk[512];
    int rc;
    if (!v->write) return OFS_NTFS_EROFS;
    if (f->root || f->first < 16) return OFS_NTFS_EROFS;
    if (f->nofat == 2 || f->nofat == 3) return OFS_NTFS_ECOMP;
    if (!v->bm_nruns) return OFS_NTFS_ENOSPACE;
    if (size == f->size) return OFS_NTFS_OK;
    if ((rc = log_reset(v))) return rc;
    if ((rc = base_data(v, f, &a))) return rc;
    v->idx_no = NONE;                           /* idx holds the runs */
    need = (ofs_u32)(((ofs_u64)size + cb - 1) / cb);
    if (!a[8]) {                                /* resident */
        voff = rd16(a + 0x14); vlen = rd32(a + 0x10);
        if (voff + vlen > rd32(a + 4)) return OFS_NTFS_EBAD;
        if ((ofs_u64)size <= v->rec_size &&
            attr_resize(v, v->rec2, a, (voff + (ofs_u32)size + 7) & ~7UL) == OFS_NTFS_OK) {
            if ((ofs_u32)size > vlen) memset(a + voff + vlen, 0, (ofs_u32)size - vlen);
            wr32(a + 0x10, (ofs_u32)size);
            f->valid = f->size = size;
            return finish(v, f, time, date, 1);
        }
        /* out of the record: its value into clusters of its own */
        if ((rc = grow_runs(v, rs, &n, 0, need))) { cut_runs(v, rs, &n, 0); return rc; }
        val = a + voff;
        for (i = 0; i < vlen; i += 512) {
            ofs_u32 k = vlen - i < 512 ? vlen - i : 512, c = i / cb, j;
            ofs_lba lba;
            for (j = 0; j < n && !(c >= rs[j].vcn && c - rs[j].vcn < rs[j].len); ++j) ;
            memcpy(blk, val + i, k);
            memset(blk + k, 0, 512 - k);
            lba = ((ofs_lba)(rs[j].lcn + (c - rs[j].vcn)) << v->cshift) + ((i >> 9) & ((1UL << v->cshift) - 1));
            if ((rc = blk_patch(v, lba, 0, blk, 512))) { cut_runs(v, rs, &n, 0); return rc; }
        }
        /* the attribute becomes non-resident: header 0x40, then the runs */
        if ((rc = attr_resize(v, v->rec2, a, 0x48))) { cut_runs(v, rs, &n, 0); return rc; }
        memset(a + 0x10, 0, 0x38);
        a[8] = 1; a[9] = 0;
        wr16(a + 0x0A, 0x40);
        wr16(a + 0x20, 0x40);
        if ((rc = put_runs(v, v->rec2, a, rs, n, size, vlen))) {
            v->rec2_no = NONE;                  /* the record in memory is spoilt: read again */
            cut_runs(v, rs, &n, 0);
            return rc;
        }
        f->nofat = 0; f->rlen = 0;
        f->size = size; f->valid = vlen;
        return finish(v, f, time, date, 1);
    }
    if ((rc = runs_get(a, rs, &n))) return rc;
    for (have = 0, i = 0; i < n; ++i) have += rs[i].len;
    init = rd64(a + 0x38);
    if (need > have) {
        if ((rc = grow_runs(v, rs, &n, have, need)) ||
            (rc = put_runs(v, v->rec2, a, rs, n, size, init))) {
            cut_runs(v, rs, &n, have);          /* the new clusters free again */
            v->rec2_no = NONE;
            return rc;
        }
    } else {
        if ((ofs_u64)size < init) init = size;
        if (need < have && (rc = cut_runs(v, rs, &n, need))) return rc;
        if ((rc = put_runs(v, v->rec2, a, rs, n, size, init))) { v->rec2_no = NONE; return rc; }
    }
    f->size = size;
    if (f->valid > size) f->valid = size;
    f->rlen = 0;
    return finish(v, f, time, date, 1);
}

int ofs_ntfs_write(struct ofs_ntfs *v, struct ofs_ntfs_file *f, ofs_fsize off, const void OFS_FAR *buf,
                   ofs_u16 len, ofs_u16 *done, ofs_u16 time, ofs_u16 date)
{
    static const ofs_u8 zeros[512];
    const ofs_u8 OFS_FAR *in8 = (const ofs_u8 OFS_FAR *)buf;
    const ofs_u8 *a;
    ofs_u8 *val, *ba;
    ofs_u32 vlen, sec, vcn;
    struct ofs_ntfs_run r;
    unsigned in, n;
    ofs_fsize pos, end;
    ofs_lba lba;
    int rc, zero;
    *done = 0;
    if (!v->write) return OFS_NTFS_EROFS;
    if (f->root || f->first < 16) return OFS_NTFS_EROFS;
    if (f->nofat == 2 || f->nofat == 3) return OFS_NTFS_ECOMP;
    if (!len) return OFS_NTFS_OK;
    if (off > f->size || len > f->size - off) {     /* past the end: the file grows first */
        if ((rc = ofs_ntfs_resize(v, f, off + len, time, date))) return rc;
    }
    if (f->nofat == 1) {                            /* resident: in the record */
        if ((rc = data_attr(v, f->first, 0, &a))) return rc;
        if (!(val = (ofs_u8 *)res_value(a, &vlen)) || off + len > vlen) return OFS_NTFS_EBAD;
        fmemcpy(val + (unsigned)off, in8, len);
        if ((rc = write_rec(v, v->rec2_no, v->rec2))) return rc;
        *done = len;
        return finish(v, f, time, date, 0);
    }
    /* from the initialized size on: zeros up to off first */
    pos = off < f->valid ? off : f->valid;
    end = off + len;
    if (end > f->valid && (rc = log_reset(v))) return rc;
    while (pos < end) {
        zero = pos < off;
        in = (unsigned)pos & 511;
        n = 512 - in;
        if (zero ? n > off - pos : n > end - pos) n = (unsigned)(zero ? off - pos : end - pos);
        sec = (ofs_u32)(pos >> 9);
        vcn = sec >> v->cshift;
        if (!f->rlen || vcn < f->idx || vcn - f->idx >= f->rlen) {
            if ((rc = data_attr(v, f->first, vcn, &a))) return rc;
            if (!a[8]) return OFS_NTFS_EBAD;
            if ((rc = attr_run(a, vcn, &r))) return rc == OFS_NTFS_END ? OFS_NTFS_EBAD : rc;
            f->idx = r.vcn; f->clu = r.lcn; f->rlen = r.len;
        }
        if (f->clu == NONE) {                       /* a hole: not yet */
            if (zero || !*done) return OFS_NTFS_ENOSPACE;
            break;
        }
        lba = ((ofs_lba)(f->clu + (vcn - f->idx)) << v->cshift) + (sec & ((1UL << v->cshift) - 1));
        if ((rc = blk_patch(v, lba, in, zero ? zeros : in8, n))) return rc;
        pos += n;
        if (!zero) { in8 += n; *done += (ofs_u16)n; }
    }
    if (pos > f->valid) {                           /* the initialized size follows */
        if ((rc = base_data(v, f, &ba))) return rc;
        wr64(ba + 0x38, (ofs_u64)pos);
        f->valid = pos;
    }
    return finish(v, f, time, date, 0);
}

/* ---- writing, stage 2b: creating ----
   A new file or directory gets a free MFT record (by $MFT's $BITMAP, from
   record 24 on as Windows and ntfs-3g keep 16-23 for $MFT's own extension
   records; $MFT grows by 16 records when none is free) and an entry in its
   directory's index.  The index is a B-tree ordered by the volume's
   $UpCase (COLLATION_FILE_NAME: case-insensitively, then exactly): the
   entry goes into its leaf; a full index block is split at its middle,
   the middle entry going up into the node above with the new block (the
   lower half) as its child; a full root (in the directory's record) moves
   into an index block of its own, the root keeping only its last entry
   pointing there ($INDEX_ALLOCATION and $BITMAP made when the directory
   has none).  Writes in that order: the record before the entry, a new
   block before the node pointing to it -- a power loss leaves at worst a
   record or a block nothing names (the check finds them).
   The name is a Win32 one, or a Win32-and-DOS one when it is a valid 8.3
   name in capitals; no DOS name of its own is made (as ntfs-3g does, and
   Windows with 8.3 names off).  The new file has its directory's security
   ($STANDARD_INFORMATION's security id); $MFTMirr gets record 0 too. */

#define ENTMAX    OFS_NTFS_ENTMAX
#define DEPTH     8                     /* index levels */
#define MFT_FIRST 24                    /* the first record for files */
#define MFT_GROW  16                    /* records $MFT grows by */
#define NOSUB     (~(ofs_u64)0)
#define UPNONE    0xFFFFU

static const ofs_u8 i30[8] = { '$', 0, 'I', 0, '3', 0, '0', 0 };

/* block blk (256 units) of $UpCase into dst */
static int up_load(struct ofs_ntfs *v, unsigned blk, ofs_u8 *dst)
{
    ofs_u32 vcn = (ofs_u32)blk >> v->cshift;
    unsigned i;
    for (i = 0; i < v->up_nruns; ++i)
        if (vcn >= v->upr[i].vcn && vcn - v->upr[i].vcn < v->upr[i].len)
            return blk_read(v, ((ofs_lba)(v->upr[i].lcn + (vcn - v->upr[i].vcn)) << v->cshift) +
                               (blk & ((1UL << v->cshift) - 1)), dst);
    return OFS_NTFS_EBAD;
}

/* c in capitals, by the volume's $UpCase */
static int up(struct ofs_ntfs *v, ofs_u16 c, ofs_u16 *u)
{
    unsigned blk = c >> 8;
    int rc;
    if (!blk) { *u = rd16(v->up0 + 2 * c); return OFS_NTFS_OK; }
    if (v->up_blk != blk) {
        v->up_blk = UPNONE;
        if ((rc = up_load(v, blk, v->up1))) return rc;
        v->up_blk = (ofs_u16)blk;
    }
    *u = rd16(v->up1 + 2 * (c & 255));
    return OFS_NTFS_OK;
}

/* $MFTMirr and $UpCase, at ofs_ntfs_set_write (neither known: no creating) */
static int create_setup(struct ofs_ntfs *v)
{
    struct ofs_ntfs_run *rs = (struct ofs_ntfs_run *)v->idx;
    const ofs_u8 *b, *a;
    unsigned n, i;
    int rc;
    v->up_nruns = 0; v->mirr_n = 0; v->up_blk = UPNONE;
    if (!(b = sector(v, 0))) return OFS_NTFS_EIO;
    if (rd32(b + 0x3C)) return OFS_NTFS_OK;
    v->mirr_lcn = rd32(b + 0x38);
    v->mirr_n = (ofs_u8)(clu_bytes(v) <= 4UL * v->rec_size ? 4 : clu_bytes(v) / v->rec_size);
    if ((rc = load_rec(v, 10, v->rec2, &v->rec2_no))) return rc == OFS_NTFS_END ? OFS_NTFS_EBAD : rc;
    if (!(a = find_attr(v, v->rec2, AT_DATA, 0)) || !a[8] || rd64(a + 0x38) < 131072UL) return OFS_NTFS_OK;
    v->idx_no = NONE;
    if (runs_get(a, rs, &n) || n > 4) return OFS_NTFS_OK;
    for (i = 0; i < n; ++i) {
        if (rs[i].lcn == NONE) return OFS_NTFS_OK;
        v->upr[i] = rs[i];
    }
    v->up_nruns = (ofs_u8)n;
    if ((rc = up_load(v, 0, v->up0))) v->up_nruns = 0;
    return rc;
}

/* COLLATION_FILE_NAME of the $FILE_NAME keys k1 and k2: *r < 0, 0, > 0;
   *ic: equal case-insensitively */
static int fn_cmp(struct ofs_ntfs *v, const ofs_u8 *k1, const ofs_u8 *k2, int *r, int *ic)
{
    unsigned n1 = k1[0x40], n2 = k2[0x40], n = n1 < n2 ? n1 : n2, i, pass;
    ofs_u16 a, b;
    int rc;
    *ic = 0;
    for (pass = 0; pass < 2; ++pass) {
        for (i = 0; i < n; ++i) {
            a = rd16(k1 + 0x42 + 2 * i); b = rd16(k2 + 0x42 + 2 * i);
            if (!pass && ((rc = up(v, a, &a)) || (rc = up(v, b, &b)))) return rc;
            if (a != b) { *r = a < b ? -1 : 1; return OFS_NTFS_OK; }
        }
        if (n1 != n2) { *r = n1 < n2 ? -1 : 1; return OFS_NTFS_OK; }
        *ic = 1;
    }
    *r = 0;
    return OFS_NTFS_OK;
}

/* 3: a valid 8.3 name in capitals (Win32 and DOS), 1: a Win32 name, -1: not valid */
static int name_ns(const ofs_u16 *s, unsigned len)
{
    static const char dos[] = "!#$%&'()-@^_`{}~";
    unsigned i, dot = len;
    int d = 1;
    ofs_u16 c;
    if (!len || len > 255 || s[len - 1] == '.' || s[len - 1] == ' ') return -1;
    for (i = 0; i < len; ++i) {
        c = s[i];
        if (c < 0x20 || c == '"' || c == '*' || c == '/' || c == ':' || c == '<' || c == '>' ||
            c == '?' || c == '\\' || c == '|') return -1;
        if (c == '.') {
            if (dot != len) d = 0;
            dot = i;
        } else if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || (c < 0x80 && strchr(dos, (char)c)))) {
            d = 0;
        }
    }
    if (!dot || dot > 8 || (dot < len && len - dot - 1 > 3)) d = 0;
    return d ? 3 : 1;
}

/* the index of a block (number) and its VCN (clusters, or 512 bytes when
   a cluster is larger than an index block) */
static unsigned vbits(const struct ofs_ntfs *v) { return clu_bytes(v) <= v->idx_size ? v->cshift + 9u : 9u; }
static ofs_u32 blk_of(const struct ofs_ntfs *v, ofs_u64 vcn) { return (ofs_u32)((vcn << vbits(v)) / v->idx_size); }
static ofs_u64 vcn_of(const struct ofs_ntfs *v, ofs_u32 k) { return ((ofs_u64)k * v->idx_size) >> vbits(v); }

/* a new attribute of type, len bytes (zeros but its type, length and id),
   into record b at its place by type */
static int attr_insert(struct ofs_ntfs *v, ofs_u8 *b, ofs_u32 type, ofs_u32 len, ofs_u8 **pa)
{
    ofs_u32 used = rd32(b + 0x18), alloc = rd32(b + 0x1C), at = rd16(b + 0x14), alen;
    if (alloc > v->rec_size || used > alloc) return OFS_NTFS_EBAD;
    if (used + len > alloc) return OFS_NTFS_ENOSPACE;
    for (; at + 8 <= used && rd32(b + at) != AT_END && rd32(b + at) <= type; at += alen) {
        alen = rd32(b + at + 4);
        if (alen < 0x18 || at + alen > used) return OFS_NTFS_EBAD;
    }
    if (at + 8 > used) return OFS_NTFS_EBAD;
    memmove(b + at + len, b + at, used - at);
    memset(b + at, 0, len);
    wr32(b + at, type);
    wr32(b + at + 4, len);
    wr16(b + at + 0x0E, rd16(b + 0x28));        /* the next attribute id */
    wr16(b + 0x28, (ofs_u16)(rd16(b + 0x28) + 1));
    wr32(b + 0x18, used + len);
    *pa = b + at;
    return OFS_NTFS_OK;
}

/* the non-resident attribute a of record b (all of it there, from VCN 0)
   grows to size bytes: clusters added when its allocation is smaller
   (runs in idx), the bytes from its initialized size on zeroed when zero;
   size and initialized size set */
static int nr_grow(struct ofs_ntfs *v, ofs_u8 *b, ofs_u8 *a, ofs_u64 size, int zero)
{
    static const ofs_u8 zeros[512];
    struct ofs_ntfs_run *rs = (struct ofs_ntfs_run *)v->idx;
    ofs_u32 cb = clu_bytes(v), have = 0, need, c, k;
    ofs_u64 pos;
    unsigned n = 0, i;
    int rc;
    if (!a[8] || rd64(a + 0x10)) return OFS_NTFS_EBAD;
    if (rd64(a + 0x28) && rd64(a + 0x18) + 1 != rd64(a + 0x28) / cb) return OFS_NTFS_ENOSPACE;
    v->idx_no = NONE;
    if ((rc = runs_get(a, rs, &n))) return rc;
    for (i = 0; i < n; ++i) {
        if (rs[i].lcn == NONE) return OFS_NTFS_ENOSPACE;
        have += rs[i].len;
    }
    need = (ofs_u32)((size + cb - 1) / cb);
    if (need > have && (rc = grow_runs(v, rs, &n, have, need))) { cut_runs(v, rs, &n, have); return rc; }
    for (pos = rd64(a + 0x38); zero && pos < size; pos += k) {
        c = (ofs_u32)(pos / cb);
        for (i = 0; i < n && !(c >= rs[i].vcn && c - rs[i].vcn < rs[i].len); ++i) ;
        if (i == n) return OFS_NTFS_EBAD;
        k = 512 - ((unsigned)pos & 511);
        if (k > size - pos) k = (ofs_u32)(size - pos);
        if ((rc = blk_patch(v, ((ofs_lba)(rs[i].lcn + (c - rs[i].vcn)) << v->cshift) +
                               (ofs_u32)((pos >> 9) & ((1UL << v->cshift) - 1)), (unsigned)pos & 511, zeros, k)))
            return rc;
    }
    if ((rc = put_runs(v, b, a, rs, n, size, size))) { cut_runs(v, rs, &n, have); return rc; }
    return OFS_NTFS_OK;
}

/* the run cache of $MFT from its $DATA a */
static int mft_runs(struct ofs_ntfs *v, const ofs_u8 *a)
{
    struct ofs_ntfs_run r;
    ofs_u32 vcn, end = rd32(a + 0x18) + 1;
    v->mft_nruns = 0; v->mft_next = 1;
    for (vcn = 0; vcn < end && v->mft_nruns < OFS_NTFS_MFTRUNS; vcn = r.vcn + r.len) {
        if (attr_run(a, vcn, &r) || r.lcn == NONE) return OFS_NTFS_EBAD;
        v->mft[v->mft_nruns++] = r;
    }
    return OFS_NTFS_OK;
}

/* the bit of record no in $MFT's $BITMAP (record 0 in rec2): on or off */
static int mft_bit(struct ofs_ntfs *v, ofs_u32 no, int on)
{
    ofs_u8 *bm, *s;
    ofs_u32 len;
    ofs_lba lba;
    int rc;
    if ((rc = load_rec(v, 0, v->rec2, &v->rec2_no))) return rc == OFS_NTFS_END ? OFS_NTFS_EBAD : rc;
    if (!(bm = (ofs_u8 *)find_attr(v, v->rec2, AT_BITMAP, 0))) return OFS_NTFS_EBAD;
    if (!bm[8]) {
        if (!(s = (ofs_u8 *)res_value(bm, &len)) || no / 8 >= len) return OFS_NTFS_EBAD;
        if (on) s[no >> 3] |= (ofs_u8)(1u << (no & 7)); else s[no >> 3] &= (ofs_u8)~(1u << (no & 7));
        return write_rec(v, 0, v->rec2);
    }
    if (no / 8 >= rd64(bm + 0x38)) return OFS_NTFS_EBAD;
    if ((rc = attr_sector(v, bm, no >> 12, &lba))) return rc;
    if (lba == NOBUF || !(s = (ofs_u8 *)sector(v, lba))) return lba == NOBUF ? OFS_NTFS_EBAD : OFS_NTFS_EIO;
    if (on) s[no >> 3 & 511] |= (ofs_u8)(1u << (no & 7)); else s[no >> 3 & 511] &= (ofs_u8)~(1u << (no & 7));
    return buf_flush(v);
}

/* $MFT grows by MFT_GROW records (a cluster at least), formatted empty;
   its $BITMAP first, so that it always covers the records */
static int mft_grow(struct ofs_ntfs *v)
{
    ofs_u8 *b = v->rec2, *d, *bm, *r = v->rec, *val;
    ofs_u32 rs = v->rec_size, cb = clu_bytes(v), old, nrec, i, bytes, len, first;
    ofs_u64 size;
    int rc;
    if ((rc = load_rec(v, 0, b, &v->rec2_no))) return rc == OFS_NTFS_END ? OFS_NTFS_EBAD : rc;
    if (!(d = (ofs_u8 *)find_attr(v, b, AT_DATA, 0)) || !d[8] || !(bm = (ofs_u8 *)find_attr(v, b, AT_BITMAP, 0)))
        return OFS_NTFS_ENOSPACE;
    old = (ofs_u32)(rd64(d + 0x30) / rs);
    size = (ofs_u64)(old + MFT_GROW) * rs;
    if (cb > rs) size = (size + cb - 1) / cb * cb;
    nrec = (ofs_u32)(size / rs);
    bytes = (nrec + 63) / 64 * 8;
    if (!bm[8]) {
        len = rd32(bm + 0x10);
        if (bytes > len) {
            if ((rc = attr_resize(v, b, bm, (rd16(bm + 0x14) + bytes + 7) & ~7UL))) goto spoilt;
            val = bm + rd16(bm + 0x14);
            memset(val + len, 0, bytes - len);
            wr32(bm + 0x10, bytes);
        }
    } else if (bytes > rd64(bm + 0x30) && (rc = nr_grow(v, b, bm, bytes, 1))) {
        goto spoilt;
    }
    d = (ofs_u8 *)find_attr(v, b, AT_DATA, 0);
    if ((rc = nr_grow(v, b, d, size, 0))) goto spoilt;
    if ((rc = mft_runs(v, d))) goto spoilt;
    v->rec_no = NONE;                           /* the new records, through rec */
    for (i = old; i < nrec; ++i) {
        memset(r, 0, rs);
        memcpy(r, "FILE", 4);
        wr16(r + 4, rd16(b + 4)); wr16(r + 6, rd16(b + 6));
        first = (rd16(b + 4) + 2u * rd16(b + 6) + 7) & ~7u;
        wr16(r + 0x14, (ofs_u16)first);
        wr32(r + 0x18, first + 8);
        wr32(r + 0x1C, rs);
        if (rd16(b + 4) >= 0x30) wr32(r + 0x2C, i);
        wr32(r + first, AT_END);
        if ((rc = write_rec(v, i, r))) return rc;
    }
    return write_rec(v, 0, b);
spoilt:
    v->rec2_no = NONE;
    if (!load_rec(v, 0, b, &v->rec2_no) && (d = (ofs_u8 *)find_attr(v, b, AT_DATA, 0))) mft_runs(v, d);
    return rc;
}

/* a free MFT record, marked in use in $MFT's $BITMAP: *no */
static int mft_alloc(struct ofs_ntfs *v, ofs_u32 *no)
{
    const ofs_u8 *d, *bm, *val, *s;
    ofs_u32 bits, i, len, blk;
    ofs_lba lba;
    int rc, pass;
    for (pass = 0; pass < 2; ++pass) {
        if ((rc = load_rec(v, 0, v->rec2, &v->rec2_no))) return rc == OFS_NTFS_END ? OFS_NTFS_EBAD : rc;
        if (!(d = find_attr(v, v->rec2, AT_DATA, 0)) || !d[8] || !(bm = find_attr(v, v->rec2, AT_BITMAP, 0)))
            return OFS_NTFS_ENOSPACE;
        bits = (ofs_u32)(rd64(d + 0x30) / v->rec_size);
        if (!bm[8]) {
            if (!(val = res_value(bm, &len))) return OFS_NTFS_EBAD;
            if (len * 8 < bits) bits = len * 8;
            for (i = MFT_FIRST; i < bits; ++i)
                if (!(val[i >> 3] >> (i & 7) & 1)) { *no = i; return mft_bit(v, i, 1); }
        } else {
            if (rd64(bm + 0x38) * 8 < bits) bits = (ofs_u32)rd64(bm + 0x38) * 8;
            for (i = MFT_FIRST; i < bits;) {
                blk = i >> 12;
                if ((rc = attr_sector(v, bm, blk, &lba))) return rc;
                if (lba == NOBUF) return OFS_NTFS_EBAD;
                if (!(s = sector(v, lba))) return OFS_NTFS_EIO;
                for (; i < bits && i >> 12 == blk; ++i)
                    if (!(s[i >> 3 & 511] >> (i & 7) & 1)) { *no = i; return mft_bit(v, i, 1); }
            }
        }
        if (pass) break;
        if ((rc = mft_grow(v))) return rc;
    }
    return OFS_NTFS_EBAD;
}

/* record no (the bit already set) made: $STANDARD_INFORMATION, the
   $FILE_NAME key (klen bytes), then $DATA (empty) or $INDEX_ROOT (empty,
   its first 16 bytes from iroot); *seq its sequence number */
static int rec_make(struct ofs_ntfs *v, ofs_u32 no, int is_dir, const ofs_u8 *key, ofs_u32 klen,
                    const ofs_u8 *iroot, ofs_u32 secid, ofs_u64 t, ofs_u16 *seq)
{
    ofs_u8 *b = v->rec2, *a, *val;
    unsigned n = v->rec_size / 512, i, usa, cnt;
    ofs_u32 p, len;
    ofs_lba lba;
    int rc, ok;
    v->rec2_no = NONE;
    for (i = 0; i < n; ++i) {
        if ((rc = mft_sector(v, no * n + i, &lba))) return rc;
        if (blk_read(v, lba, b + i * 512)) return OFS_NTFS_EIO;
    }
    ok = !memcmp(b, "FILE", 4) && !fixup(b, v->rec_size);
    if (ok && (rd16(b + 0x16) & 1)) return OFS_NTFS_EBAD;  /* free by the bitmap, but in use */
    *seq = ok ? rd16(b + 0x10) : 0;
    if (!*seq) *seq = 1;
    usa = ok ? rd16(b + 4) : (v->ver_minor ? 0x30u : 0x2Au);
    cnt = n + 1;
    memset(b, 0, v->rec_size);
    memcpy(b, "FILE", 4);
    wr16(b + 4, (ofs_u16)usa); wr16(b + 6, (ofs_u16)cnt);
    wr16(b + 0x10, *seq);
    wr16(b + 0x12, 1);                          /* links */
    wr16(b + 0x16, (ofs_u16)(is_dir ? 3 : 1));
    wr32(b + 0x1C, v->rec_size);
    if (usa >= 0x30) wr32(b + 0x2C, no);
    p = (usa + 2 * cnt + 7) & ~7u;
    wr16(b + 0x14, (ofs_u16)p);
    a = b + p;                                  /* $STANDARD_INFORMATION */
    wr32(a, AT_STD); wr32(a + 4, 0x60); wr32(a + 0x10, 0x48); wr16(a + 0x14, 0x18);
    val = a + 0x18;
    for (i = 0; i < 4; ++i) wr64(val + 8 * i, t);
    wr32(val + 0x20, is_dir ? 0 : OFS_NTFS_ARCH);
    wr32(val + 0x34, secid);
    p += 0x60;
    a = b + p;                                  /* $FILE_NAME */
    len = (0x18 + klen + 7) & ~7UL;
    wr32(a, AT_FNAME); wr32(a + 4, len); wr16(a + 0x0E, 1);
    wr32(a + 0x10, klen); wr16(a + 0x14, 0x18); a[0x16] = 1;   /* indexed */
    memcpy(a + 0x18, key, klen);
    p += len;
    a = b + p;
    if (!is_dir) {                              /* $DATA */
        wr32(a, AT_DATA); wr32(a + 4, 0x18); wr16(a + 0x0E, 2); wr16(a + 0x14, 0x18);
        p += 0x18;
    } else {                                    /* $INDEX_ROOT $I30 */
        wr32(a, AT_IROOT); wr32(a + 4, 0x50); a[9] = 4; wr16(a + 0x0A, 0x18); wr16(a + 0x0E, 2);
        wr32(a + 0x10, 0x30); wr16(a + 0x14, 0x20);
        memcpy(a + 0x18, i30, 8);
        val = a + 0x20;
        memcpy(val, iroot, 16);
        wr32(val + 0x10, 0x10); wr32(val + 0x14, 0x20); wr32(val + 0x18, 0x20);
        wr16(val + 0x20 + 8, 16); wr16(val + 0x20 + 12, 2);    /* the last entry */
        p += 0x50;
    }
    wr32(b + p, AT_END);
    wr32(b + 0x18, p + 8);
    wr16(b + 0x28, 3);                          /* the next attribute id */
    return write_rec(v, no, b);
}

/* record no free again (its bit off, not in use, the next sequence number) */
static int mft_free(struct ofs_ntfs *v, ofs_u32 no)
{
    ofs_u16 seq;
    int rc;
    if ((rc = load_rec(v, no, v->rec2, &v->rec2_no))) return rc == OFS_NTFS_END ? mft_bit(v, no, 0) : rc;
    wr16(v->rec2 + 0x16, (ofs_u16)(rd16(v->rec2 + 0x16) & ~1u));
    seq = (ofs_u16)(rd16(v->rec2 + 0x10) + 1);
    wr16(v->rec2 + 0x10, seq ? seq : 1);
    if ((rc = write_rec(v, no, v->rec2))) return rc;
    v->rec2_no = NONE;
    return mft_bit(v, no, 0);
}

/* the index node k of directory dir: NONE the root (in rec), else block k
   (in idx); *base its index header, *limit the bytes it may hold, *ra the
   root's attribute */
static int node_get(struct ofs_ntfs *v, ofs_u32 dir, ofs_u32 k, ofs_u8 **base, ofs_u32 *limit, ofs_u8 **ra)
{
    const ofs_u8 *a, *val;
    ofs_u32 len;
    int rc;
    if (k == NONE) {
        if ((rc = ext_attr(v, dir, AT_IROOT, 1, 0, v->rec, &v->rec_no, &a)))
            return rc == OFS_NTFS_END ? OFS_NTFS_EBAD : rc;
        if (v->rec_no != dir) return OFS_NTFS_ENOSPACE;   /* in an extension record */
        if (!(val = res_value(a, &len)) || len < 32) return OFS_NTFS_EBAD;
        *base = (ofs_u8 *)val + 16; *limit = len - 16; *ra = (ofs_u8 *)a;
    } else {
        rc = load_idx(v, dir, k);
        if (rc == UNUSED || rc == OFS_NTFS_END) return OFS_NTFS_EBAD;
        if (rc) return rc;
        *base = v->idx + 24; *limit = v->idx_size - 24u; *ra = 0;
    }
    if (rd32(*base) < 16 || rd32(*base) > rd32(*base + 4) || rd32(*base + 4) > *limit) return OFS_NTFS_EBAD;
    return OFS_NTFS_OK;
}

/* where key goes in the node at base: *at the entry it goes before, *sub
   that entry's child (NOSUB: a leaf); EEXIST when exist and the name is
   there (case-insensitively) */
static int node_find(struct ofs_ntfs *v, const ofs_u8 *base, const ofs_u8 *key, int exist, ofs_u32 *at, ofs_u64 *sub)
{
    ofs_u32 used = rd32(base + 4), off, elen, klen, fl;
    const ofs_u8 *p;
    int r, ic, rc;
    for (off = rd32(base);; off += elen) {
        if (off + 16 > used) return OFS_NTFS_EBAD;
        p = base + off;
        elen = rd16(p + 8); klen = rd16(p + 10); fl = rd16(p + 12);
        if (elen < 16 || (elen & 7) || off + elen > used || ((fl & 1) && elen < 24)) return OFS_NTFS_EBAD;
        if (!(fl & 2)) {
            if (klen < 0x42 || 16 + klen + ((fl & 1) ? 8 : 0) > elen || 0x42u + 2u * p[16 + 0x40] > klen)
                return OFS_NTFS_EBAD;
            if ((rc = fn_cmp(v, key, p + 16, &r, &ic))) return rc;
            if (ic && exist) return OFS_NTFS_EEXIST;
            if (r > 0) continue;
        }
        *at = off;
        *sub = (fl & 1) ? rd64(p + elen - 8) : NOSUB;
        return OFS_NTFS_OK;
    }
}

/* the entry in ent into node k of dir, where its key goes; ENOSPACE when
   the node has no room; *pos (pos not 0) where it went */
static int node_put(struct ofs_ntfs *v, ofs_u32 dir, ofs_u32 k, ofs_u32 *pos)
{
    ofs_u8 *base, *ra;
    ofs_u32 limit, at, used, esize = rd16(v->ent + 8);
    ofs_u64 sub;
    int rc;
    if ((rc = node_get(v, dir, k, &base, &limit, &ra))) return rc;
    if ((rc = node_find(v, base, v->ent + 16, 0, &at, &sub))) return rc;
    used = rd32(base + 4);
    if (k == NONE) {
        if ((rc = attr_resize(v, v->rec, ra, rd32(ra + 4) + esize))) return rc;
        memmove(base + at + esize, base + at, used - at);
        memcpy(base + at, v->ent, esize);
        wr32(ra + 0x10, rd32(ra + 0x10) + esize);
        wr32(base + 4, used + esize);
        wr32(base + 8, rd32(base + 8) + esize);
        rc = write_rec(v, dir, v->rec);
    } else {
        if (rd32(base + 8) > limit) return OFS_NTFS_EBAD;
        if (used + esize > rd32(base + 8)) return OFS_NTFS_ENOSPACE;
        memmove(base + at + esize, base + at, used - at);
        memcpy(base + at, v->ent, esize);
        wr32(base + 4, used + esize);
        rc = write_idx(v);
    }
    if (!rc && pos) *pos = (k == NONE ? 0 : k + 1) << 16 | at;
    return rc;
}

/* a free index block in the directory record b (in memory; not written):
   *k and its blocks (512 bytes) in lbas; $INDEX_ALLOCATION and $BITMAP
   made when the directory has none, the allocation grown when no block is
   free (runs in idx) */
static int idx_alloc_in(struct ofs_ntfs *v, ofs_u8 *b, ofs_u32 *k, ofs_lba *lbas)
{
    ofs_u8 *ia, *bm, *val;
    ofs_u32 len, blocks, nb, i, s;
    int rc;
    ia = (ofs_u8 *)find_attr(v, b, AT_IALLOC, 1);
    bm = (ofs_u8 *)find_attr(v, b, AT_BITMAP, 1);
    if (!ia != !bm) return OFS_NTFS_ENOSPACE;   /* one of them in an extension record */
    if (!ia) {
        if (find_attr(v, b, AT_LIST, 0)) return OFS_NTFS_ENOSPACE;
        if ((rc = attr_insert(v, b, AT_IALLOC, 0x50, &ia))) return rc;
        ia[8] = 1; ia[9] = 4; wr16(ia + 0x0A, 0x40); wr16(ia + 0x20, 0x48);
        wr64(ia + 0x18, ~(ofs_u64)0);           /* no clusters yet */
        memcpy(ia + 0x40, i30, 8);
        if ((rc = attr_insert(v, b, AT_BITMAP, 0x28, &bm))) return rc;
        bm[9] = 4; wr16(bm + 0x0A, 0x18); wr32(bm + 0x10, 8); wr16(bm + 0x14, 0x20);
        memcpy(bm + 0x18, i30, 8);
        ia = (ofs_u8 *)find_attr(v, b, AT_IALLOC, 1);
    }
    if (bm[8] || !ia[8]) return OFS_NTFS_ENOSPACE;   /* a non-resident $BITMAP: not yet */
    blocks = (ofs_u32)(rd64(ia + 0x30) / v->idx_size);
    val = bm + rd16(bm + 0x14);
    len = rd32(bm + 0x10);
    nb = len * 8;
    for (i = 0; i < blocks && i < nb && (val[i >> 3] >> (i & 7) & 1); ++i) ;
    if (i >= nb) {                              /* the bitmap grows by 8 bytes */
        if ((rc = attr_resize(v, b, bm, (rd16(bm + 0x14) + len + 8 + 7) & ~7UL))) return rc;
        memset(bm + rd16(bm + 0x14) + len, 0, 8);
        wr32(bm + 0x10, len + 8);
    }
    if (i >= blocks) {                          /* a new block at the end */
        ia = (ofs_u8 *)find_attr(v, b, AT_IALLOC, 1);
        if ((rc = nr_grow(v, b, ia, (ofs_u64)(i + 1) * v->idx_size, 0))) return rc;
    }
    bm = (ofs_u8 *)find_attr(v, b, AT_BITMAP, 1);
    val = bm + rd16(bm + 0x14);
    val[i >> 3] |= (ofs_u8)(1u << (i & 7));
    ia = (ofs_u8 *)find_attr(v, b, AT_IALLOC, 1);
    for (s = 0; s < v->idx_size / 512u; ++s) {
        if ((rc = attr_sector(v, ia, (ofs_u32)((ofs_u64)i * v->idx_size >> 9) + s, &lbas[s]))) return rc;
        if (lbas[s] == NOBUF) return OFS_NTFS_EBAD;
    }
    *k = i;
    return OFS_NTFS_OK;
}

/* the same, the record of dir through rec2, written */
static int idx_alloc(struct ofs_ntfs *v, ofs_u32 dir, ofs_u32 *k, ofs_lba *lbas)
{
    int rc;
    if ((rc = load_rec(v, dir, v->rec2, &v->rec2_no))) return rc == OFS_NTFS_END ? OFS_NTFS_EBAD : rc;
    if ((rc = idx_alloc_in(v, v->rec2, k, lbas))) { v->rec2_no = NONE; return rc; }
    return write_rec(v, dir, v->rec2);
}

/* the root of dir (full) moves into a new index block *k; the root keeps
   its last entry only, pointing there.  The record is full, so the block
   is made in it after the root shrank: the block's image in rec2 first,
   then the root cut, the block allocated in the record, the block written,
   then the record */
static int root_down(struct ofs_ntfs *v, ofs_u32 dir, ofs_u32 *k)
{
    ofs_lba lbas[OFS_NTFS_MAXREC / 512];
    ofs_u8 *base, *ra, *h = v->rec2, *p;
    ofs_u32 limit, first, used, bfirst, cnt, vlen;
    int rc;
    if ((rc = node_get(v, dir, NONE, &base, &limit, &ra))) return rc;
    first = rd32(base); used = rd32(base + 4);
    v->rec2_no = NONE;                          /* the block's image */
    memset(h, 0, v->idx_size);
    memcpy(h, "INDX", 4);
    cnt = v->idx_size / 512u + 1;
    wr16(h + 4, 0x28); wr16(h + 6, (ofs_u16)cnt);
    bfirst = ((0x28 + 2 * cnt + 7) & ~7UL) - 0x18;
    if (bfirst + used - first > v->idx_size - 0x18u) return OFS_NTFS_EBAD;
    memcpy(h + 0x18 + bfirst, base + first, used - first);
    wr32(h + 0x18, bfirst);
    wr32(h + 0x1C, bfirst + used - first);
    wr32(h + 0x20, v->idx_size - 0x18u);
    h[0x24] = (ofs_u8)(base[12] & 1);
    p = base + first;                           /* the root: its last entry, to the block */
    memset(p, 0, 24);
    wr16(p + 8, 24); wr16(p + 12, 3);
    wr32(base + 4, first + 24); wr32(base + 8, first + 24);
    base[12] |= 1;
    vlen = 16 + first + 24;
    wr32(ra + 0x10, vlen);
    if ((rc = attr_resize(v, v->rec, ra, (rd16(ra + 0x14) + vlen + 7) & ~7UL)) ||
        (rc = idx_alloc_in(v, v->rec, k, lbas))) {
        v->rec_no = NONE;                       /* spoilt: read again */
        return rc;
    }
    ra = (ofs_u8 *)find_attr(v, v->rec, AT_IROOT, 1);
    wr64(ra + rd16(ra + 0x14) + 16 + first + 16, vcn_of(v, *k));
    wr64(h + 0x10, vcn_of(v, *k));
    memcpy(v->idx, h, v->idx_size);
    memcpy(v->idx_lba, lbas, sizeof(lbas));
    v->idx_dir = dir; v->idx_no = *k;
    if ((rc = write_idx(v))) return rc;
    return write_rec(v, dir, v->rec);
}

/* block k of dir (full) split at its middle entry: the entries before it
   into a new block, those after it stay; the entry in ent goes into its
   half (*pos where), then ent holds the middle entry with the new block
   as its child, for the node above */
static int node_split(struct ofs_ntfs *v, ofs_u32 dir, ofs_u32 k, ofs_u32 *pos)
{
    ofs_lba lbas[OFS_NTFS_MAXREC / 512];
    ofs_u8 *base, *ra, *p;
    ofs_u32 kl, limit, first, used, off, elen, moff = 0, mlen = 0, n, m, endlen;
    int rc, inner, r, ic;
    if ((rc = idx_alloc(v, dir, &kl, lbas))) return rc;
    if ((rc = node_get(v, dir, k, &base, &limit, &ra))) return rc;
    first = rd32(base); used = rd32(base + 4);
    inner = base[12] & 1;
    for (n = 0, off = first; off + 16 <= used && !(rd16(base + off + 12) & 2); off += elen, ++n) {
        elen = rd16(base + off + 8);
        if (elen < 16 || off + elen > used) return OFS_NTFS_EBAD;
    }
    if (n < 3) return OFS_NTFS_EBAD;
    /* the middle: the entry across half of the bytes, not the first nor the last */
    for (m = 0, off = first; m < n; off += elen, ++m) {
        elen = rd16(base + off + 8);
        if ((m >= 1 && off + elen > first + (used - first) / 2) || m == n - 2) { moff = off; mlen = elen; break; }
    }
    if (mlen > ENTMAX) return OFS_NTFS_EBAD;
    memcpy(v->ment, base + moff, mlen);
    /* the lower half, into the new block */
    endlen = inner ? 24 : 16;
    p = base + moff;
    memset(p, 0, used - moff);
    wr16(p + 8, (ofs_u16)endlen);
    wr16(p + 12, (ofs_u16)(inner ? 3 : 2));
    if (inner) wr64(p + 16, rd64(v->ment + mlen - 8));
    wr32(base + 4, moff + endlen);
    wr64(v->idx + 0x10, vcn_of(v, kl));
    memcpy(v->idx_lba, lbas, sizeof(lbas));
    v->idx_no = kl;
    if ((rc = write_idx(v))) return rc;
    /* the upper half stays in k */
    v->idx_no = NONE;
    if ((rc = node_get(v, dir, k, &base, &limit, &ra))) return rc;
    if (rd32(base + 4) != used) return OFS_NTFS_EBAD;
    memmove(base + first, base + moff + mlen, used - moff - mlen);
    memset(base + used - (moff + mlen - first), 0, moff + mlen - first);
    wr32(base + 4, used - (moff + mlen - first));
    if ((rc = write_idx(v))) return rc;
    /* the entry into its half */
    if ((rc = fn_cmp(v, v->ent + 16, v->ment + 16, &r, &ic))) return rc;
    rc = node_put(v, dir, r < 0 ? kl : k, pos);
    if (rc) return rc == OFS_NTFS_ENOSPACE ? OFS_NTFS_EBAD : rc;
    /* the middle entry, with the new block as its child, for the node above */
    if (!inner) {
        mlen += 8;
        wr16(v->ment + 8, (ofs_u16)mlen);
        wr16(v->ment + 12, (ofs_u16)(rd16(v->ment + 12) | 1));
    }
    wr64(v->ment + mlen - 8, vcn_of(v, kl));
    memcpy(v->ent, v->ment, mlen);
    return OFS_NTFS_OK;
}

/* the entry in ent into the index of dir; *pos where it went */
static int idx_insert(struct ofs_ntfs *v, ofs_u32 dir, ofs_u32 *pos)
{
    ofs_u32 path[DEPTH], node = NONE, at, limit;
    ofs_u8 *base, *ra;
    ofs_u64 sub;
    unsigned depth = 0;
    int rc, mine = 1;
    for (;;) {                                  /* down to the leaf */
        if ((rc = node_get(v, dir, node, &base, &limit, &ra))) return rc;
        if ((rc = node_find(v, base, v->ent + 16, 1, &at, &sub))) return rc;
        if (sub == NOSUB) break;
        if (depth == DEPTH) return OFS_NTFS_EBAD;
        path[depth++] = node;
        node = blk_of(v, sub);
    }
    for (;;) {                                  /* and up while nodes are full */
        rc = node_put(v, dir, node, mine ? pos : 0);
        if (rc != OFS_NTFS_ENOSPACE) return rc;
        if (node == NONE) {
            if ((rc = root_down(v, dir, &node))) return rc;
            path[0] = NONE; depth = 1;
            continue;
        }
        if ((rc = node_split(v, dir, node, mine ? pos : 0))) return rc;
        mine = 0;
        if (!depth) return OFS_NTFS_EBAD;
        node = path[--depth];
    }
}

int ofs_ntfs_create(struct ofs_ntfs *v, struct ofs_ntfs_file *dir, const ofs_u16 *name, unsigned len, int is_dir,
                    ofs_u16 time, ofs_u16 date, ofs_u32 *pos)
{
    ofs_u8 *k = v->ent + 16, iroot[16];
    const ofs_u8 *a, *val;
    ofs_u32 klen, no, secid = 0, vlen, i;
    ofs_u16 pseq, seq;
    ofs_u64 t;
    int ns, rc;
    if (!v->write) return OFS_NTFS_EROFS;
    if (!v->bm_nruns || !v->up_nruns || v->idx_size < 2048) return OFS_NTFS_ENOSPACE;
    if ((ns = name_ns(name, len)) < 0) return OFS_NTFS_ENAME;
    if ((rc = load_rec(v, dir->first, v->rec2, &v->rec2_no))) return rc == OFS_NTFS_END ? OFS_NTFS_EBAD : rc;
    if (!(rd16(v->rec2 + 0x16) & 2)) return OFS_NTFS_EBAD;
    pseq = rd16(v->rec2 + 0x10);
    if ((a = find_attr(v, v->rec2, AT_STD, 0)) && (val = res_value(a, &vlen)) && vlen >= 0x38) secid = rd32(val + 0x34);
    if (!(a = find_attr(v, v->rec2, AT_IROOT, 1)) || !(val = res_value(a, &vlen)) || vlen < 16)
        return OFS_NTFS_ENOSPACE;
    memcpy(iroot, val, 16);
    /* the entry: its key, the $FILE_NAME */
    t = nt_time(time, date, v->tz);
    memset(v->ent, 0, ENTMAX);
    klen = 0x42 + 2 * len;
    wr32(k, dir->first); wr16(k + 6, pseq);
    for (i = 0; i < 4; ++i) wr64(k + 8 + 8 * i, t);
    wr32(k + 0x38, is_dir ? F_DIR : OFS_NTFS_ARCH);
    k[0x40] = (ofs_u8)len; k[0x41] = (ofs_u8)ns;
    for (i = 0; i < len; ++i) wr16(k + 0x42 + 2 * i, name[i]);
    wr16(v->ent + 8, (ofs_u16)(16 + ((klen + 7) & ~7UL)));
    wr16(v->ent + 10, (ofs_u16)klen);
    if ((rc = log_reset(v))) return rc;
    if ((rc = mft_alloc(v, &no))) return rc;
    if ((rc = rec_make(v, no, is_dir, k, klen, iroot, secid, t, &seq))) { mft_bit(v, no, 0); return rc; }
    wr32(v->ent, no); wr16(v->ent + 6, seq);
    if ((rc = idx_insert(v, dir->first, pos))) { mft_free(v, no); return rc; }
    return finish(v, dir, time, date, 0);
}
#endif
