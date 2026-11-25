/* SPDX-License-Identifier: GPL-2.0-or-later */
/* see include/ofs/fat.h: OS/2's and Windows NT's extended attributes on
   FAT12/16.  The layout of "EA DATA. SF" (as OS/2 and NT's fastfat keep
   it; own code):

     0     the file header, 512 bytes: "ED", format and log words (0),
           at 32 EaBaseTable[240], cluster numbers within the file
     512   the offset table: one word per handle, FFFFh unused; a handle
           h's set starts at cluster EaBaseTable[h >> 7] + offset[h]; the
           table ends where EaBaseTable[0] begins (the first set)
     ...   the sets, each in whole clusters, one after the other:
           "EA", its own handle, the count of EAs flagged "need" (80h),
           the owner's 8.3 name (14 bytes), 4 reserved, cbList (4 bytes,
           counting itself), then the EAs: flags, name length, value
           length (word), the name, a NUL, the value

   Handles are 1 .. 30719.  A new file gets an offset table for all of
   them, so that it never has to be rebuilt. */
#include <string.h>
#include "ofs/fat.h"

#ifdef __WATCOMC__
#define fmemcpy _fmemcpy
#else
#define fmemcpy memcpy
#endif

#define HDR      512UL
#define SETHDR   30UL
#define CBLIST   26UL
#define HANDLES  30720UL           /* handle 0 is never used */
#define UNUSED   0xFFFFu
#define NEED     0x80

static const char ea_fcb[11] = { 'E', 'A', ' ', 'D', 'A', 'T', 'A', ' ', ' ', 'S', 'F' };  /* "EA DATA. SF" */

static ofs_u8 b[512];              /* work buffer */
static struct ofs_fat_ent fe;      /* the EA file's entry */
static struct ofs_fat_file root, ef;
static struct ofs_fat_where ew;
static ofs_u32 bpc;                /* bytes per cluster */
static ofs_u32 entries;            /* handles the offset table holds */

static unsigned rd16(const ofs_u8 *p) { return (unsigned)(p[0] | p[1] << 8); }
static void wr16(ofs_u8 *p, unsigned v) { p[0] = (ofs_u8)v; p[1] = (ofs_u8)(v >> 8); }

static int get(struct ofs_fat *v, ofs_u32 off, void OFS_FAR *buf, ofs_u32 len)
{
    ofs_u16 n, done;
    int r;
    while (len) {
        n = len > 0x4000UL ? 0x4000 : (ofs_u16)len;
        if ((r = ofs_fat_read(v, &ef, off, buf, n, &done)) != 0) return r;
        if (done != n) return OFS_FAT_EBAD;
        off += n;
        buf = (ofs_u8 OFS_FAR *)buf + n;
        len -= n;
    }
    return OFS_FAT_OK;
}

static int put(struct ofs_fat *v, ofs_u32 off, const void OFS_FAR *buf, ofs_u32 len)
{
    ofs_u16 n, done;
    int r;
    while (len) {
        n = len > 0x4000UL ? 0x4000 : (ofs_u16)len;
        if ((r = ofs_fat_write(v, &ef, &ew, off, buf, n, &done)) != 0) return r;
        if (done != n) return OFS_FAT_EFULL;
        off += n;
        buf = (const ofs_u8 OFS_FAR *)buf + n;
        len -= n;
    }
    return OFS_FAT_OK;
}

static int get16(struct ofs_fat *v, ofs_u32 off, unsigned *x)
{
    ofs_u8 t[2];
    int r = get(v, off, t, 2);
    *x = rd16(t);
    return r;
}

static int put16(struct ofs_fat *v, ofs_u32 off, unsigned x)
{
    ofs_u8 t[2];
    wr16(t, x);
    return put(v, off, t, 2);
}

/* the EA file: OFS_FAT_END when there is none */
static int open_file(struct ofs_fat *v)
{
    unsigned base0;
    int r;
    if (v->bits == 32) return OFS_FAT_ENOTSUP;
    bpc = 1UL << (v->sshift + v->cshift);
    ofs_fat_rootdir(v, &root);
    if ((r = ofs_fat_lookup(v, &root, ea_fcb, &fe)) != 0) return r;
    ef = fe.file;
    ofs_fat_where_of(&root, fe.pos, &ew);
    if (ef.size < HDR || (r = get(v, 0, b, 34)) != 0) return r ? r : OFS_FAT_EBAD;
    base0 = rd16(b + 32);
    if (b[0] != 'E' || b[1] != 'D' || base0 * bpc < HDR || base0 * bpc > ef.size) return OFS_FAT_EBAD;
    entries = (base0 * bpc - HDR) / 2;
    if (entries > HANDLES) entries = HANDLES;
    return OFS_FAT_OK;
}

/* a new EA file: the header, every handle unused */
static int make_file(struct ofs_fat *v, ofs_u16 time, ofs_u16 date)
{
    ofs_u32 pos, base0, off;
    unsigned i;
    int r;
    if ((r = ofs_fat_create_fcb(v, &root, ea_fcb, OFS_FAT_HIDDEN | OFS_FAT_SYSTEM | OFS_FAT_RDONLY,
                                time, date, &pos)) != 0) return r;
    if ((r = open_file(v)) != OFS_FAT_EBAD) return r ? r : OFS_FAT_EBAD;   /* empty: EBAD */
    base0 = (HDR + 2 * HANDLES + bpc - 1) / bpc;
    memset(b, 0, sizeof(b));
    b[0] = 'E'; b[1] = 'D';
    for (i = 0; i < 240; ++i) wr16(b + 32 + 2 * i, (unsigned)base0);
    if ((r = put(v, 0, b, HDR)) != 0) return r;
    memset(b, 0xFF, sizeof(b));
    for (off = HDR; off < base0 * bpc; off += sizeof(b))
        if ((r = put(v, off, b, base0 * bpc - off < sizeof(b) ? base0 * bpc - off : sizeof(b))) != 0) return r;
    return open_file(v);
}

/* the cluster of handle h's set, 0 when it has none */
static int set_of(struct ofs_fat *v, unsigned h, ofs_u32 *cl)
{
    unsigned base, o;
    int r;
    *cl = 0;
    if (!h || h >= entries) return OFS_FAT_OK;
    if ((r = get16(v, 32 + 2UL * (h >> 7), &base)) != 0 || (r = get16(v, HDR + 2UL * h, &o)) != 0) return r;
    if (o != UNUSED) *cl = (ofs_u32)base + o;
    return OFS_FAT_OK;
}

/* points handle h to the set at cluster cl (0: unused) */
static int point(struct ofs_fat *v, unsigned h, ofs_u32 cl)
{
    unsigned base;
    int r;
    if (!cl) return put16(v, HDR + 2UL * h, UNUSED);
    if ((r = get16(v, 32 + 2UL * (h >> 7), &base)) != 0) return r;
    if (cl < base || cl - base >= UNUSED) return OFS_FAT_EFULL;
    return put16(v, HDR + 2UL * h, (unsigned)(cl - base));
}

/* the set header at cluster cl: its handle and cbList */
static int set_head(struct ofs_fat *v, ofs_u32 cl, unsigned *h, ofs_u32 *cb)
{
    int r;
    if (cl * bpc + SETHDR > ef.size) return OFS_FAT_EBAD;
    if ((r = get(v, cl * bpc, b, SETHDR)) != 0) return r;
    *h = rd16(b + 2);
    *cb = rd16(b + CBLIST) | (ofs_u32)rd16(b + CBLIST + 2) << 16;
    if (b[0] != 'E' || b[1] != 'A' || *cb < 4 || cl * bpc + CBLIST + *cb > ef.size) return OFS_FAT_EBAD;
    return OFS_FAT_OK;
}

static ofs_u32 set_clusters(ofs_u32 cb) { return (CBLIST + cb + bpc - 1) / bpc; }

static int same(const char *a, const ofs_u8 *x, unsigned n)
{
    unsigned i;
    for (i = 0; i < n; ++i) {
        unsigned c = (ofs_u8)a[i], d = x[i];
        if (c >= 'a' && c <= 'z') c -= 32;
        if (d >= 'a' && d <= 'z') d -= 32;
        if (c != d) return 0;
    }
    return !a[n];
}

/* the next EA from *off (before end): its flags, name (in b + 4, NUL at
   its end) and value length; *off moves past it */
static int next_ea(struct ofs_fat *v, ofs_u32 *off, ofs_u32 end, unsigned *nl, unsigned *vl, ofs_u32 *at)
{
    int r;
    if (*off + 4 > end) return OFS_FAT_EBAD;
    if ((r = get(v, *off, b, 4)) != 0) return r;
    *nl = b[1];
    *vl = rd16(b + 2);
    if (*off + 5 + *nl + *vl > end) return OFS_FAT_EBAD;
    if ((r = get(v, *off + 4, b + 4, *nl)) != 0) return r;
    b[4 + *nl] = 0;
    *at = *off;
    *off += 5UL + *nl + *vl;
    return OFS_FAT_OK;
}

/* the EA name in the set at cl (cbList cb): its offset and size, 0 none */
static int find(struct ofs_fat *v, ofs_u32 cl, ofs_u32 cb, const char *name, ofs_u32 *at, ofs_u32 *size)
{
    ofs_u32 off = cl * bpc + SETHDR, end = cl * bpc + CBLIST + cb, a;
    unsigned nl, vl;
    int r;
    *size = 0;
    while (off < end) {
        if ((r = next_ea(v, &off, end, &nl, &vl, &a)) != 0) return r;
        if (same(name, b + 4, nl)) {
            *at = a;
            *size = off - a;
            return OFS_FAT_OK;
        }
    }
    return OFS_FAT_OK;
}

static int set_of_entry(struct ofs_fat *v, unsigned h, ofs_u32 *cl, ofs_u32 *cb)
{
    unsigned own;
    int r = open_file(v);
    *cl = 0;
    if (r == OFS_FAT_END) return OFS_FAT_OK;
    if (r || (r = set_of(v, h, cl)) != 0 || !*cl) return r;
    if ((r = set_head(v, *cl, &own, cb)) != 0) return r;
    return own == h ? OFS_FAT_OK : OFS_FAT_EBAD;
}

int ofs_fat_ea_get(struct ofs_fat *v, const struct ofs_fat_ent *e, const char *name, void OFS_FAR *buf,
                    ofs_u16 size, ofs_u16 *len)
{
    ofs_u32 cl, cb, at, sz;
    int r;
    *len = 0;
    if (v->bits == 32) return OFS_FAT_ENOTSUP;
    if (!e->ea) return OFS_FAT_ENOATTR;
    if ((r = set_of_entry(v, e->ea, &cl, &cb)) != 0) return r;
    if (!cl) return OFS_FAT_ENOATTR;
    if ((r = find(v, cl, cb, name, &at, &sz)) != 0) return r;
    if (!sz) return OFS_FAT_ENOATTR;
    if ((r = get(v, at, b, 4)) != 0) return r;
    *len = (ofs_u16)rd16(b + 2);
    if (!size) return OFS_FAT_OK;
    if (*len > size) return OFS_FAT_ERANGE;
    return get(v, at + 5 + b[1], buf, *len);
}

int ofs_fat_ea_list(struct ofs_fat *v, const struct ofs_fat_ent *e, char OFS_FAR *buf, ofs_u16 size,
                     ofs_u16 *len)
{
    ofs_u32 cl, cb, off, end, a, total = 0;
    unsigned nl, vl;
    int r;
    *len = 0;
    if (v->bits == 32) return OFS_FAT_ENOTSUP;
    if (!e->ea) return OFS_FAT_OK;
    if ((r = set_of_entry(v, e->ea, &cl, &cb)) != 0 || !cl) return r;
    off = cl * bpc + SETHDR;
    end = cl * bpc + CBLIST + cb;
    while (off < end) {
        if ((r = next_ea(v, &off, end, &nl, &vl, &a)) != 0) return r;
        if (size) {
            if (total + nl + 1 > size) return OFS_FAT_ERANGE;
            fmemcpy(buf + total, b + 4, nl + 1);
        }
        total += nl + 1;
        if (total > 0xFFFFUL) return OFS_FAT_ERANGE;
    }
    *len = (ofs_u16)total;
    return OFS_FAT_OK;
}

/* copies len bytes inside the EA file from src to dst (dst < src, or
   regions apart) */
static int copy(struct ofs_fat *v, ofs_u32 dst, ofs_u32 src, ofs_u32 len)
{
    ofs_u32 n;
    int r;
    while (len) {
        n = len > sizeof(b) ? sizeof(b) : len;
        if ((r = get(v, src, b, n)) != 0 || (r = put(v, dst, b, n)) != 0) return r;
        src += n; dst += n; len -= n;
    }
    return OFS_FAT_OK;
}

/* squeezes out the sets no handle points to, from cluster cl on: each set
   behind is moved down and its handle pointed to it at once, so a crash
   leaves every handle on a whole set; the file is cut last */
static int squeeze(struct ofs_fat *v, ofs_u32 cl)
{
    ofs_u32 end = ef.size / bpc, shift = 0, cb, n, at;
    unsigned h;
    int r;
    while (cl < end) {
        if ((r = set_head(v, cl, &h, &cb)) != 0) break;   /* damaged: leave the rest */
        n = set_clusters(cb);
        if ((r = set_of(v, h, &at)) != 0) return r;
        if (at != cl) shift += n;                       /* nobody's */
        else if (shift) {
            if ((r = copy(v, (cl - shift) * bpc, cl * bpc, n * bpc)) != 0) return r;
            if ((r = point(v, h, cl - shift)) != 0) return r;
        }
        cl += n;
    }
    if (!shift || cl != end) return OFS_FAT_OK;
    return ofs_fat_resize(v, &ef, &ew, (end - shift) * bpc);
}

int ofs_fat_ea_drop(struct ofs_fat *v, ofs_u16 handle)
{
    ofs_u32 cl, cb;
    int r = set_of_entry(v, handle, &cl, &cb);
    if (r || !cl) return r;
    if ((r = point(v, handle, 0)) != 0) return r;
    return squeeze(v, cl);
}

/* a free handle */
static int new_handle(struct ofs_fat *v, unsigned *h)
{
    ofs_u32 off;
    unsigned i;
    int r;
    for (off = HDR; off < HDR + 2 * entries; off += sizeof(b)) {
        ofs_u32 n = HDR + 2 * entries - off < sizeof(b) ? HDR + 2 * entries - off : sizeof(b);
        if ((r = get(v, off, b, n)) != 0) return r;
        for (i = 0; i < n; i += 2)
            if (rd16(b + i) == UNUSED && off + i > HDR) {
                *h = (unsigned)((off + i - HDR) / 2);
                return OFS_FAT_OK;
            }
    }
    return OFS_FAT_EFULL;
}

int ofs_fat_ea_set(struct ofs_fat *v, struct ofs_fat_file *dir, const struct ofs_fat_ent *e, const char *name,
                    const void OFS_FAR *val, ofs_u16 vlen, int how, ofs_u16 time, ofs_u16 date)
{
    ofs_u32 pos = e->pos, cl = 0, cb = 4, at = 0, sz = 0, ncb, ncl, off, end, a, need = 0;
    unsigned old = e->ea, h = e->ea, nl = (unsigned)strlen(name), enl, evl, i, k;
    char owner[14];
    int r;
    if (v->bits == 32) return OFS_FAT_ENOTSUP;
    if (!nl || nl > 255) return OFS_FAT_ENAME;
    /* the owner's 8.3 name, "NAME.EXT" */
    memset(owner, 0, sizeof(owner));
    for (i = 0, k = 0; i < 8 && e->fcb[i] != ' '; ++i) owner[k++] = e->fcb[i];
    if (e->fcb[8] != ' ') {
        owner[k++] = '.';
        for (i = 8; i < 11 && e->fcb[i] != ' '; ++i) owner[k++] = e->fcb[i];
    }
    if (h && (r = set_of_entry(v, h, &cl, &cb)) != 0) return r;
    if (cl && (r = find(v, cl, cb, name, &at, &sz)) != 0) return r;
    if (how == OFS_FAT_EA_CREATE && sz) return OFS_FAT_EEXIST;
    if ((how == OFS_FAT_EA_REPLACE || how == OFS_FAT_EA_REMOVE) && !sz) return OFS_FAT_ENOATTR;
    ncb = cb - sz + (how == OFS_FAT_EA_REMOVE ? 0 : 5UL + nl + vlen);
    if (ncb > 0xFFFFUL) return OFS_FAT_EFULL;
    if (ncb == 4) {
        /* the last one gone: no set, no handle */
        if ((r = point(v, h, 0)) != 0 || (r = ofs_fat_set_ea(v, dir, pos, 0)) != 0) return r;
        return squeeze(v, cl);
    }
    r = open_file(v);
    if (r == OFS_FAT_END) r = make_file(v, time, date);
    if (r) return r;
    if (!cl && (r = new_handle(v, &h)) != 0) return r;
    /* the new set at the end: the kept EAs, then this one */
    ncl = (ef.size + bpc - 1) / bpc;
    off = ncl * bpc + SETHDR;
    if (cl) {
        ofs_u32 src = cl * bpc + SETHDR;
        end = cl * bpc + CBLIST + cb;
        while (src < end) {
            if ((r = next_ea(v, &src, end, &enl, &evl, &a)) != 0) return r;
            if (a == at && sz) continue;
            if (b[0] & NEED) ++need;
            if ((r = copy(v, off, a, src - a)) != 0) return r;
            off += src - a;
        }
    }
    if (how != OFS_FAT_EA_REMOVE) {
        b[0] = 0;
        b[1] = (ofs_u8)nl;
        wr16(b + 2, vlen);
        memcpy(b + 4, name, nl + 1);
        if ((r = put(v, off, b, 5UL + nl)) != 0 || (r = put(v, off + 5 + nl, val, vlen)) != 0) return r;
        off += 5UL + nl + vlen;
    }
    /* the rest of its last cluster, then the header */
    memset(b, 0, sizeof(b));
    while (off % bpc) {
        a = bpc - off % bpc > sizeof(b) ? sizeof(b) : bpc - off % bpc;
        if ((r = put(v, off, b, a)) != 0) return r;
        off += a;
    }
    b[0] = 'E'; b[1] = 'A';
    wr16(b + 2, h);
    wr16(b + 4, (unsigned)need);
    memcpy(b + 8, owner, sizeof(owner));
    wr16(b + CBLIST, (unsigned)(ncb & 0xFFFF));
    wr16(b + CBLIST + 2, 0);
    if ((r = put(v, ncl * bpc, b, SETHDR)) != 0) return r;
    /* the handle to it (one write), the entry to the handle, the old set out */
    if ((r = point(v, h, ncl)) != 0) return r;
    if (h != old && (r = ofs_fat_set_ea(v, dir, pos, (ofs_u16)h)) != 0) return r;
    return cl ? squeeze(v, cl) : OFS_FAT_OK;
}
