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

static int mft_sector(struct ofat_ntfs *v, ofat_u32 s, ofat_lba *lba);
static int attr_is(const ofat_u8 *a, ofat_u32 len, ofat_u32 type, int i30, ofat_u32 vcn);
static const ofat_u8 *find_attr(const struct ofat_ntfs *v, const ofat_u8 *b, ofat_u32 type, int i30);
static const ofat_u8 *res_value(const ofat_u8 *a, ofat_u32 *len);

/* record no of $MFT into idx through the runs known so far (for
   mft_sector: no new lookups from here) */
static int mft_raw(struct ofat_ntfs *v, ofat_u32 no)
{
    unsigned n = v->rec_size / 512, i;
    ofat_lba lba;
    int r;
    v->idx_no = NONE;
    for (i = 0; i < n; ++i) {
        if ((r = mft_sector(v, no * n + i, &lba))) return r;
        if (v->buf_lba == lba) v->buf_lba = NOBUF;
        if (v->read(v->dev, v->start + lba, v->idx + i * 512)) return OFAT_NTFS_EIO;
    }
    if (memcmp(v->idx, "FILE", 4) || fixup(v->idx, v->rec_size) || !(rd16(v->idx + 0x16) & 1))
        return OFAT_NTFS_EBAD;
    return OFAT_NTFS_OK;
}

/* the run of $MFT's data with vcn from its runlist (record 0, or the
   record its attribute list names), into the cache */
static int mft_lookup(struct ofat_ntfs *v, ofat_u32 vcn)
{
    const ofat_u8 *a, *l, *end, *val, *rend;
    ofat_u32 alen, len, target = NONE;
    struct ofat_ntfs_run r;
    int rc, pass;
    for (pass = 0; pass < 2; ++pass) {
        if ((rc = mft_raw(v, pass ? target : 0))) return rc;
        rend = v->idx + v->rec_size;
        for (a = v->idx + rd16(v->idx + 0x14); a + 0x18 <= rend && rd32(a) != AT_END; a += alen) {
            alen = rd32(a + 4);
            if (alen < 0x18 || alen > (ofat_u32)(rend - a)) return OFAT_NTFS_EBAD;
            if (attr_is(a, alen, AT_DATA, 0, vcn) && a[8]) {
                if ((rc = attr_run(a, vcn, &r))) return rc == OFAT_NTFS_END ? OFAT_NTFS_EBAD : rc;
                if (r.lcn == NONE) return OFAT_NTFS_EBAD;
                if (v->mft_nruns < OFAT_NTFS_MFTRUNS) v->mft[v->mft_nruns++] = r;
                else {
                    v->mft[v->mft_next] = r;    /* the first run stays */
                    if (++v->mft_next == OFAT_NTFS_MFTRUNS) v->mft_next = 1;
                }
                return OFAT_NTFS_OK;
            }
        }
        /* not in this record: the attribute list of record 0 */
        if (pass || !(l = find_attr(v, v->idx, AT_LIST, 0))) break;
        if (l[8]) {                             /* non-resident: into idx */
            ofat_u8 la[128];
            ofat_u32 i;
            ofat_lba lba;
            if ((alen = rd32(l + 4)) > sizeof(la) || rd32(l + 0x34) || (len = rd32(l + 0x30)) > OFAT_NTFS_MAXREC)
                return OFAT_NTFS_ESIZE;
            memcpy(la, l, alen);
            for (i = 0; i * 512 < len; ++i) {
                if ((rc = attr_sector(v, la, i, &lba))) return rc;
                if (lba == NOBUF) return OFAT_NTFS_EBAD;
                if (v->buf_lba == lba) v->buf_lba = NOBUF;
                if (v->read(v->dev, v->start + lba, v->idx + i * 512)) return OFAT_NTFS_EIO;
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
    return OFAT_NTFS_EBAD;
}

static int mft_sector(struct ofat_ntfs *v, ofat_u32 s, ofat_lba *lba)
{
    ofat_u32 vcn = s >> v->cshift;
    unsigned i, tries;
    int rc;
    for (tries = 0; tries < 2; ++tries) {
        for (i = 0; i < v->mft_nruns; ++i)
            if (vcn >= v->mft[i].vcn && vcn - v->mft[i].vcn < v->mft[i].len) {
                *lba = ((ofat_lba)(v->mft[i].lcn + (vcn - v->mft[i].vcn)) << v->cshift) + (s & ((1UL << v->cshift) - 1));
                return OFAT_NTFS_OK;
            }
        if (tries || v->mft_busy) break;
        v->mft_busy = 1;
        rc = mft_lookup(v, vcn);
        v->mft_busy = 0;
        if (rc) return rc;
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
    ofat_u32 alen = rd32(a + 4), voff = rd16(a + 0x14);
    *len = rd32(a + 0x10);
    if (a[8] || voff > alen || *len > alen - voff) return 0;    /* no wrap */
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
    v->mft_nruns = 1; v->mft_next = 1;
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

/* Does the attribute a of a record match type, name ("$I30" when i30, else
   none) and, when non-resident, cover vcn? */
static int attr_is(const ofat_u8 *a, ofat_u32 len, ofat_u32 type, int i30, ofat_u32 vcn)
{
    if (rd32(a) != type) return 0;
    if (i30 ? a[9] != 4 || (ofat_u32)rd16(a + 0x0A) + 8 > len ||
              memcmp(a + rd16(a + 0x0A), "$\0I\0" "3\0" "0\0", 8) : a[9] != 0) return 0;
    return !a[8] || (len >= 0x40 && vcn >= rd32(a + 0x10) && vcn <= rd32(a + 0x18));
}

/* The attribute of type (named "$I30" when i30) of record no that covers
   vcn (resident ones: any vcn), through the attribute list when the record
   has one (a non-resident list is read into idx): *pa points into b, which
   then holds the record it is in (*cached its number).  END when the
   record has no such attribute. */
static int ext_attr(struct ofat_ntfs *v, ofat_u32 no, ofat_u32 type, int i30, ofat_u32 vcn,
                    ofat_u8 *b, ofat_u32 *cached, const ofat_u8 **pa)
{
    const ofat_u8 *a, *l, *end, *val, *rend;
    ofat_u32 len, target = NONE, alen;
    int rc, any = 0;
    if ((rc = load_rec(v, no, b, cached))) return rc == OFAT_NTFS_END ? OFAT_NTFS_EBAD : rc;
    for (a = b + rd16(b + 0x14), rend = b + v->rec_size; a + 0x18 <= rend && rd32(a) != AT_END; a += alen) {
        alen = rd32(a + 4);
        if (alen < 0x18 || alen > (ofat_u32)(rend - a)) return OFAT_NTFS_EBAD;
        if (attr_is(a, alen, type, i30, vcn)) { *pa = a; return OFAT_NTFS_OK; }
    }
    if (!(l = find_attr(v, b, AT_LIST, 0))) return OFAT_NTFS_END;
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
    /* the last entry of the type that starts at or before vcn */
    for (l = val, end = val + len; l + 0x1A <= end && rd16(l + 4) >= 0x1A && l + rd16(l + 4) <= end; l += rd16(l + 4)) {
        if (rd32(l) != type) continue;
        if (i30 ? l[6] != 4 || l + l[7] + 8 > end || memcmp(l + l[7], "$\0I\0" "3\0" "0\0", 8) : l[6] != 0)
            continue;
        any = 1;
        if (rd32(l + 0x0C) || rd32(l + 8) > vcn) break;
        target = rd32(l + 0x10);
    }
    if (target == NONE) return any ? OFAT_NTFS_EBAD : OFAT_NTFS_END;
    if (target == no) return OFAT_NTFS_EBAD;   /* it was not in the record */
    if ((rc = load_rec(v, target, b, cached))) return rc == OFAT_NTFS_END ? OFAT_NTFS_EBAD : rc;
    for (a = b + rd16(b + 0x14); a + 0x18 <= rend && rd32(a) != AT_END; a += alen) {
        alen = rd32(a + 4);
        if (alen < 0x18 || alen > (ofat_u32)(rend - a)) return OFAT_NTFS_EBAD;
        if (attr_is(a, alen, type, i30, vcn)) { *pa = a; return OFAT_NTFS_OK; }
    }
    return OFAT_NTFS_EBAD;
}

/* the unnamed $DATA of record no covering vcn, in rec2 */
static int data_attr(struct ofat_ntfs *v, ofat_u32 no, ofat_u32 vcn, const ofat_u8 **pa)
{
    int rc = ext_attr(v, no, AT_DATA, 0, vcn, v->rec2, &v->rec2_no, pa);
    return rc == OFAT_NTFS_END ? OFAT_NTFS_EBAD : rc;
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
    struct ofat_ntfs *v;
    const ofat_u32 *lcn;                        /* the unit's clusters */
    ofat_u32 pos, end;                          /* bytes in the stream */
    const ofat_u8 *s;                           /* sector spos of it */
    ofat_u32 spos;
};

/* the next byte of the stream; -1 at its end, -2 read error */
static int cbyte(struct cstream *c)
{
    ofat_u32 sec;
    ofat_lba lba;
    if (c->pos >= c->end) return -1;
    sec = c->pos >> 9;
    if (!c->s || c->spos != sec) {
        lba = ((ofat_lba)c->lcn[sec >> c->v->cshift] << c->v->cshift) + (sec & ((1UL << c->v->cshift) - 1));
        if (!(c->s = sector(c->v, lba))) return -2;
        c->spos = sec;
    }
    return c->s[(unsigned)(c->pos++ & 511)];
}

#define CBYTE(c, b) if (((b) = cbyte(c)) < 0) return (b) == -2 ? OFAT_NTFS_EIO : OFAT_NTFS_EBAD

/* the chunk with header hdr that ends at cend into out (4096 bytes) */
static int lznt1(struct cstream *c, ofat_u8 *out, unsigned hdr, ofat_u32 cend)
{
    unsigned dst = 0, tag, bit, tok, lg, i, pt, n;
    int b, b2;
    if (!(hdr & 0x8000)) {                      /* stored */
        if ((hdr & 0xFFF) != CHUNK - 1) return OFAT_NTFS_EBAD;
        for (; dst < CHUNK; ++dst) { CBYTE(c, b); out[dst] = (ofat_u8)b; }
        return OFAT_NTFS_OK;
    }
    while (c->pos < cend && dst < CHUNK) {
        CBYTE(c, b);
        tag = (unsigned)b;
        for (bit = 0; bit < 8 && c->pos < cend && dst < CHUNK; ++bit, tag >>= 1) {
            if (!(tag & 1)) { CBYTE(c, b); out[dst++] = (ofat_u8)b; continue; }
            CBYTE(c, b); CBYTE(c, b2);
            tok = (unsigned)b | (unsigned)b2 << 8;
            if (!dst) return OFAT_NTFS_EBAD;
            /* the more is decompressed, the more bits the offset gets */
            for (i = dst - 1, lg = 0; i >= 0x10; i >>= 1) ++lg;
            pt = (tok >> (12 - lg)) + 1;
            n = (tok & (0xFFFu >> lg)) + 3;
            if (pt > dst) return OFAT_NTFS_EBAD;
            for (; n && dst < CHUNK; --n, ++dst) out[dst] = out[dst - pt];
        }
    }
    if (dst < CHUNK) memset(out + dst, 0, CHUNK - dst);
    return OFAT_NTFS_OK;
}

/* chunk k (the bytes from k * 4096) of the compressed file f into idx */
static int comp_chunk(struct ofat_ntfs *v, struct ofat_ntfs_file *f, ofat_u32 k)
{
    ofat_u32 lcn[CU_CLU], first, vcn, sec;
    unsigned i, nalloc, per_cu = 2u << v->cshift, j;
    struct ofat_ntfs_run r;
    struct cstream c;
    const ofat_u8 *a;
    ofat_lba lba;
    int rc, b, b2;
    unsigned hdr;
    ofat_u32 chunk = k;
    if (v->idx_dir == f->first && v->idx_no == k) return OFAT_NTFS_OK;
    first = k / per_cu * CU_CLU;
    for (i = 0; i < CU_CLU; ++i) {
        vcn = first + i;
        if (!f->rlen || vcn < f->idx || vcn - f->idx >= f->rlen) {
            if ((rc = data_attr(v, f->first, vcn, &a))) return rc;
            if (!a[8]) return OFAT_NTFS_EBAD;
            if ((rc = attr_run(a, vcn, &r))) return rc == OFAT_NTFS_END ? OFAT_NTFS_EBAD : rc;
            f->idx = r.vcn; f->clu = r.lcn; f->rlen = r.len;
        }
        lcn[i] = f->clu == NONE ? NONE : f->clu + (vcn - f->idx);
    }
    for (nalloc = 0; nalloc < CU_CLU && lcn[nalloc] != NONE; ++nalloc) ;
    for (i = nalloc; i < CU_CLU; ++i) if (lcn[i] != NONE) return OFAT_NTFS_EBAD;
    v->idx_no = NONE;
    k %= per_cu;                                /* the chunk in the unit */
    if (nalloc == CU_CLU) {                     /* stored plain */
        for (j = 0; j < CHUNK / 512; ++j) {
            sec = k * (CHUNK / 512) + j;
            lba = ((ofat_lba)lcn[sec >> v->cshift] << v->cshift) + (sec & ((1UL << v->cshift) - 1));
            if (v->buf_lba == lba) v->buf_lba = NOBUF;
            if (v->read(v->dev, v->start + lba, v->idx + j * 512)) return OFAT_NTFS_EIO;
        }
    } else {
        c.v = v; c.lcn = lcn; c.pos = 0; c.end = (ofat_u32)nalloc << (v->cshift + 9); c.s = 0; c.spos = 0;
        for (;;) {
            if (!nalloc || c.pos + 2 > c.end) hdr = 0;
            else { CBYTE(&c, b); CBYTE(&c, b2); hdr = (unsigned)b | (unsigned)b2 << 8; }
            if (!hdr) { memset(v->idx, 0, CHUNK); break; }
            if (c.pos + (hdr & 0xFFF) + 1 > c.end) return OFAT_NTFS_EBAD;
            if (!k) {
                if ((rc = lznt1(&c, v->idx, hdr, c.pos + (hdr & 0xFFF) + 1))) return rc;
                break;
            }
            c.pos += (hdr & 0xFFF) + 1;
            --k;
        }
    }
    v->idx_dir = f->first; v->idx_no = chunk;
    return OFAT_NTFS_OK;
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
    if (f->nofat == 1) {                            /* resident */
        if ((rc = data_attr(v, f->first, 0, &a))) return rc;
        if (!(val = res_value(a, &vlen)) || off + len > vlen) return OFAT_NTFS_EBAD;
        fmemcpy(out, val + (unsigned)off, len);
        *done = len;
        return OFAT_NTFS_OK;
    }
    while (len) {
        in = (unsigned)off & (f->nofat == 3 ? CHUNK - 1 : 511);
        n = (f->nofat == 3 ? CHUNK : 512) - in;
        if (n > len) n = len;
        if (off >= f->valid) {
            fmemset(out, 0, n);
        } else if (f->nofat == 3) {                 /* compressed */
            if (f->valid - off < n) n = (unsigned)(f->valid - off);
            if ((rc = comp_chunk(v, f, (ofat_u32)(off >> 12)))) return rc;
            fmemcpy(out, v->idx + in, n);
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
    /* encrypted, or compressed other than LZNT1 in units of 16 clusters
       of 4 KB at most: not read */
    if (fl & 0x4000 || (fl & 0xFF && ((fl & 0xFF) != 1 || a[0x22] != 4 || v->cshift > 3))) f->nofat = 2;
    else if (fl & 0xFF) f->nofat = 3;           /* compressed (LZNT1) */
    /* the size within the allocated size (holes of sparse files count) */
    if (rd32(a + 0x34) > rd32(a + 0x2C) || (rd32(a + 0x34) == rd32(a + 0x2C) && rd32(a + 0x30) > rd32(a + 0x28)))
        return OFAT_NTFS_EBAD;
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
    ofat_u32 len, s, n = v->idx_size / 512, i, blocks, vcn;
    ofat_lba lba, lbas[OFAT_NTFS_MAXREC / 512];
    int rc;
    if (v->idx_dir == dir && v->idx_no == k) return OFAT_NTFS_OK;
    rc = ext_attr(v, dir, AT_IALLOC, 1, 0, v->rec, &v->rec_no, &a);
    if (rc == OFAT_NTFS_END) return OFAT_NTFS_END;
    if (rc) return rc;
    if (!a[8]) return OFAT_NTFS_END;
    blocks = rd32(a + 0x34) ? NONE : rd32(a + 0x30) / v->idx_size;
    if (k >= blocks) return OFAT_NTFS_END;
    rc = ext_attr(v, dir, AT_BITMAP, 1, (k / 4096) >> v->cshift, v->rec, &v->rec_no, &bm);
    if (!rc) {
        if (!bm[8]) {
            if (!(val = res_value(bm, &len))) return OFAT_NTFS_EBAD;
            if (k / 8 < len && !(val[k / 8] >> (k & 7) & 1)) return UNUSED;
        } else {
            if ((rc = attr_sector(v, bm, k / 4096, &lba))) return rc;
            if (lba == NOBUF) return UNUSED;
            if (!(val = sector(v, lba))) return OFAT_NTFS_EIO;
            if (!(val[k / 8 % 512] >> (k & 7) & 1)) return UNUSED;
        }
    } else if (rc != OFAT_NTFS_END) return rc;
    if ((rc = ext_attr(v, dir, AT_IALLOC, 1, (k * n) >> v->cshift, v->rec, &v->rec_no, &a)))
        return rc == OFAT_NTFS_END ? OFAT_NTFS_EBAD : rc;
    /* the sectors of the block first (finding another extent may read a
       non-resident attribute list into idx), then the block */
    s = k * n;
    for (i = 0; i < n; ++i) {
        vcn = (s + i) >> v->cshift;
        if (!a[8] || vcn < rd32(a + 0x10) || vcn > rd32(a + 0x18))
            if ((rc = ext_attr(v, dir, AT_IALLOC, 1, vcn, v->rec, &v->rec_no, &a)))
                return rc == OFAT_NTFS_END ? OFAT_NTFS_EBAD : rc;
        if ((rc = attr_sector(v, a, s + i, &lbas[i]))) return rc;
        if (lbas[i] == NOBUF) return OFAT_NTFS_EBAD;
    }
    v->idx_no = NONE;
    for (i = 0; i < n; ++i) {
        if (v->buf_lba == lbas[i]) v->buf_lba = NOBUF;
        if (v->read(v->dev, v->start + lbas[i], v->idx + i * 512)) return OFAT_NTFS_EIO;
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
            if ((rc = ext_attr(v, dir->first, AT_IROOT, 1, 0, v->rec, &v->rec_no, &a)))
                return rc == OFAT_NTFS_END ? OFAT_NTFS_EBAD : rc;
            if (!(val = res_value(a, &len)) || len < 32) return OFAT_NTFS_EBAD;
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

/* ---- 8.3 names ---- */

/* Calls fn for each $FILE_NAME of record no (also those in other records
   of its attribute list) until it returns 1; the value is in rec2. */
typedef int (*fname_fn)(struct ofat_ntfs *, const ofat_u8 *val, ofat_u32 len, void *ctx);

static int fnames_in(struct ofat_ntfs *v, fname_fn fn, void *ctx)
{
    const ofat_u8 *a = v->rec2 + rd16(v->rec2 + 0x14), *end = v->rec2 + v->rec_size, *val;
    ofat_u32 alen, len;
    for (; a + 0x18 <= end && rd32(a) != AT_END; a += alen) {
        alen = rd32(a + 4);
        if (alen < 0x18 || alen > (ofat_u32)(end - a)) return OFAT_NTFS_EBAD;
        if (rd32(a) == AT_FNAME && (val = res_value(a, &len)) && len >= 0x42 &&
            0x42u + 2u * val[0x40] <= len && fn(v, val, len, ctx)) return 1;
    }
    return 0;
}

#define FN_EXT 8                                /* records of a list looked at */

static int each_fname(struct ofat_ntfs *v, ofat_u32 no, fname_fn fn, void *ctx)
{
    const ofat_u8 *l, *val, *end;
    ofat_u32 len, ext[FN_EXT], t;
    unsigned n = 0, i;
    int rc;
    if ((rc = load_rec(v, no, v->rec2, &v->rec2_no))) return rc;
    if ((rc = fnames_in(v, fn, ctx))) return rc;
    if (!(l = find_attr(v, v->rec2, AT_LIST, 0))) return 0;
    if (l[8]) {                                 /* non-resident: into idx */
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
    for (l = val, end = val + len; l + 0x1A <= end && rd16(l + 4) >= 0x1A && l + rd16(l + 4) <= end; l += rd16(l + 4)) {
        if (rd32(l) != AT_FNAME || (t = rd32(l + 0x10)) == no) continue;
        for (i = 0; i < n && ext[i] != t; ++i) ;
        if (i == n && n < FN_EXT) ext[n++] = t;
    }
    for (i = 0; i < n; ++i) {
        if ((rc = load_rec(v, ext[i], v->rec2, &v->rec2_no))) return rc == OFAT_NTFS_END ? OFAT_NTFS_EBAD : rc;
        if ((rc = fnames_in(v, fn, ctx))) return rc;
    }
    return 0;
}

/* the 8.3 name of a long name without a DOS name: as much of its basis as
   fits before "~" and the record number in base 36 (unique, and the
   record number leads ofat_ntfs_lookup straight to the file) */
static void gen_name(char fcb[11], ofat_u32 no)
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

struct twin { ofat_u32 dir; char *fcb; };

/* a DOS name (name space 2) of the file in the directory */
static int find_twin(struct ofat_ntfs *v, const ofat_u8 *val, ofat_u32 len, void *ctx)
{
    struct twin *t = (struct twin *)ctx;
    ofat_u16 name[12];
    unsigned i, n = val[0x40];
    (void)len;
    if (val[0x41] != 2 || rd32(val) != t->dir || rd16(val + 4) || n > 12) return 0;
    for (i = 0; i < n; ++i) name[i] = rd16(val + 0x42 + 2 * i);
    ofat_cp_basis(v->cp, name, n, t->fcb);
    return 1;
}

int ofat_ntfs_short(struct ofat_ntfs *v, struct ofat_ntfs_file *d, const struct ofat_ntfs_ent *ent, char fcb[11])
{
    struct twin t;
    int rc;
    if (!ofat_cp_basis(v->cp, ent->name, ent->name_len, fcb)) return OFAT_NTFS_OK;
    t.dir = d->first; t.fcb = fcb;              /* the 8.3 twin Windows made */
    if ((rc = each_fname(v, ent->file.first, find_twin, &t)) == 1) return OFAT_NTFS_OK;
    if (rc && rc != OFAT_NTFS_END) return rc;
    gen_name(fcb, ent->file.first);
    return OFAT_NTFS_OK;
}

struct gen { ofat_u32 dir, no; const char *fcb; struct ofat_ntfs_ent *ent; int twin, found; };

/* a long name of the file in the directory whose made 8.3 name is fcb
   (into ent); a DOS name there means the file has no made name */
static int find_gen(struct ofat_ntfs *v, const ofat_u8 *val, ofat_u32 len, void *ctx)
{
    struct gen *g = (struct gen *)ctx;
    ofat_u16 name[255];
    char fcb[11];
    unsigned i, n = val[0x40];
    (void)len;
    if (rd32(val) != g->dir || rd16(val + 4)) return 0;
    if (val[0x41] == 2) { g->twin = 1; return 1; }
    for (i = 0; i < n; ++i) name[i] = rd16(val + 0x42 + 2 * i);
    if (g->found || !ofat_cp_basis(v->cp, name, n, fcb)) return 0;
    gen_name(fcb, g->no);
    if (memcmp(fcb, g->fcb, 11)) return 0;
    g->found = 1;
    g->ent->name_len = (ofat_u8)n;
    g->ent->ns = val[0x41];
    memcpy(g->ent->name, name, n * sizeof(name[0]));
    g->ent->file.first = g->no;
    return 0;
}

/* a long name of the file in the directory (for a DOS name found) */
static int find_long(struct ofat_ntfs *v, const ofat_u8 *val, ofat_u32 len, void *ctx)
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
static ofat_u32 gen_no(const char fcb[11])
{
    ofat_u32 no = 0;
    unsigned i = 8, digits = 0;
    while (i && fcb[i-1] == ' ') --i;
    while (i && fcb[i-1] != '~') {
        --i;
        if (!(fcb[i] >= '0' && fcb[i] <= '9') && !(fcb[i] >= 'A' && fcb[i] <= 'Z')) return NONE;
        ++digits;
    }
    if (!i || !digits || digits > 6) return NONE;
    for (; i < 8 && fcb[i] != ' '; ++i)
        no = no * 36 + (ofat_u32)(fcb[i] <= '9' ? fcb[i] - '0' : fcb[i] - 'A' + 10);
    return no;
}

int ofat_ntfs_lookup(struct ofat_ntfs *v, struct ofat_ntfs_file *d, const char fcb[11], struct ofat_ntfs_ent *ent)
{
    char name[11];
    ofat_u32 pos = 0, no;
    struct gen g;
    int r;
    g.dir = d->first; g.fcb = fcb; g.ent = ent; g.twin = g.found = 0;
    /* a made name: its record, no search */
    if ((no = gen_no(fcb)) != NONE && no >= 16) {
        g.no = no;
        r = each_fname(v, no, find_gen, &g);
        if (r == OFAT_NTFS_EIO) return r;
        if (r == 0 && g.found) goto found;
        /* else a real 8.3 name with "~", or no such record */
        g.found = g.twin = 0;
    }
    /* else the 8.3 names as they are, and the DOS names Windows made */
    for (;;) {
        r = iterate(v, d, &pos, ent, IT_DOS);
        if (r) return r;
        if (ofat_cp_basis(v->cp, ent->name, ent->name_len, name) || memcmp(name, fcb, 11)) continue;
        if (ent->ns == 2) {                     /* its long name */
            g.no = ent->file.first;
            if ((r = each_fname(v, g.no, find_long, &g)) < 0) return r == OFAT_NTFS_END ? OFAT_NTFS_EBAD : r;
            if (!r) return OFAT_NTFS_EBAD;
            ent->file.first = g.no;
        }
        break;
    }
found:
    no = ent->file.first;
    r = stat_rec(v, no, ent);
    ent->pos = 0;
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
