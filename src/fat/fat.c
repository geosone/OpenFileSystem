/* FAT12/16/32 reading and writing with VFAT long names; see
   include/ofs/fat.h. */
#include <string.h>
#include "ofs/fat.h"

#ifdef __WATCOMC__
#define fmemcpy _fmemcpy
#define fmemset _fmemset
#else
#define fmemcpy memcpy
#define fmemset memset
#endif

#define NOBUF 0xFFFFFFFFUL
#define CB(v) ((v)->sshift + (v)->cshift)      /* bytes per cluster, a shift */

static ofs_u16 rd16(const ofs_u8 OFS_FAR *p) { return (ofs_u16)(p[0] | p[1] << 8); }
static ofs_u32 rd32(const ofs_u8 OFS_FAR *p) { return rd16(p) | (ofs_u32)rd16(p + 2) << 16; }
static void wr16(ofs_u8 *p, ofs_u16 v) { p[0] = (ofs_u8)v; p[1] = (ofs_u8)(v >> 8); }
static void wr32(ofs_u8 *p, ofs_u32 v) { wr16(p, (ofs_u16)v); wr16(p + 2, (ofs_u16)(v >> 16)); }

/* scratch entries: the library is not reentrant (like the exFAT code) and
   keeps the DOS stack small */
static struct ofs_fat_ent tmp;
static ofs_u8 se[32];

/* ---- sectors ---- */

static ofs_u8 *sector(struct ofs_fat *v, ofs_u32 lba)
{
    if (v->buf_lba != lba) {
        v->buf_lba = NOBUF;
        if (v->read(v->dev, v->start + lba, v->buf)) return NULL;
        v->buf_lba = lba;
    }
    return v->buf;
}

/* writes buf (holding lba, changed by the caller) */
static int put(struct ofs_fat *v, ofs_u32 lba)
{
    if (!v->write) return OFS_FAT_EWRITE;
    if (v->write(v->dev, v->start + lba, v->buf)) { v->buf_lba = NOBUF; return OFS_FAT_EWRITE; }
    return OFS_FAT_OK;
}

/* ---- the FAT ---- */

static ofs_u32 eoc(const struct ofs_fat *v)
{
    return v->bits == 12 ? 0xFFFUL : v->bits == 16 ? 0xFFFFUL : 0x0FFFFFFFUL;
}

static int valid(const struct ofs_fat *v, ofs_u32 c) { return c >= 2 && c <= v->clusters + 1; }
static int at_end(const struct ofs_fat *v, ofs_u32 c) { return c >= (eoc(v) & ~7UL); }

/* the byte at off of FAT copy k */
static int fat_byte(struct ofs_fat *v, unsigned k, ofs_u32 off, ofs_u8 *b)
{
    const ofs_u8 *s = sector(v, v->fat + k * v->fat_size + (off >> v->sshift));
    if (!s) return OFS_FAT_EIO;
    *b = s[(unsigned)off & ((1u << v->sshift) - 1)];
    return OFS_FAT_OK;
}

static ofs_u32 entry_off(const struct ofs_fat *v, ofs_u32 c)
{
    return v->bits == 12 ? c + c / 2 : c * (v->bits / 8);
}

static int fat_get(struct ofs_fat *v, ofs_u32 c, ofs_u32 *val)
{
    ofs_u32 off = entry_off(v, c);
    ofs_u8 b[4];
    unsigned i, n = v->bits == 32 ? 4 : 2;
    int r;
    for (i = 0; i < n; ++i)
        if ((r = fat_byte(v, v->active, off + i, &b[i])) != 0) return r;
    if (v->bits == 12) *val = c & 1 ? rd16(b) >> 4 : rd16(b) & 0xFFF;
    else if (v->bits == 16) *val = rd16(b);
    else *val = rd32(b) & 0x0FFFFFFFUL;
    return OFS_FAT_OK;
}

static int fat_set(struct ofs_fat *v, ofs_u32 c, ofs_u32 val)
{
    ofs_u32 off = entry_off(v, c), lba;
    ofs_u8 b[4], *s;
    unsigned i, k, n = v->bits == 32 ? 4 : 2, mask = (1u << v->sshift) - 1;
    int r;
    for (i = 0; i < n; ++i)
        if ((r = fat_byte(v, v->active, off + i, &b[i])) != 0) return r;
    if (v->bits == 12) {
        ofs_u16 w = rd16(b);
        w = c & 1 ? (ofs_u16)((w & 0x000F) | (ofs_u16)(val << 4)) : (ofs_u16)((w & 0xF000) | (val & 0xFFF));
        wr16(b, w);
    } else if (v->bits == 16) wr16(b, (ofs_u16)val);
    else wr32(b, (rd32(b) & 0xF0000000UL) | (val & 0x0FFFFFFFUL));
    for (k = 0; k < v->nfats; ++k) {
        if (!v->mirror && k != v->active) continue;
        for (i = 0; i < n; ) {
            lba = v->fat + k * v->fat_size + ((off + i) >> v->sshift);
            if (!(s = sector(v, lba))) return OFS_FAT_EIO;
            do { s[(unsigned)(off + i) & mask] = b[i]; ++i; }
            while (i < n && ((off + i) >> v->sshift) == ((off + i - 1) >> v->sshift));
            if ((r = put(v, lba)) != 0) return r;
        }
    }
    return OFS_FAT_OK;
}

/* the cluster number idx of a chain */
static int walk(struct ofs_fat *v, struct ofs_fat_file *f, ofs_u32 idx, ofs_u32 *clu)
{
    ofs_u32 i = 0, c = f->first, next;
    int r;
    if (f->clu && f->idx <= idx) { i = f->idx; c = f->clu; }
    if (!valid(v, c)) return c ? OFS_FAT_EBAD : OFS_FAT_END;
    while (i < idx) {
        if ((r = fat_get(v, c, &next)) != 0) return r;
        if (at_end(v, next)) return OFS_FAT_END;
        if (!valid(v, next) || i > v->clusters) return OFS_FAT_EBAD;
        c = next; ++i;
    }
    f->idx = i; f->clu = c;
    *clu = c;
    return OFS_FAT_OK;
}

static ofs_u32 cluster_lba(const struct ofs_fat *v, ofs_u32 c)
{
    return v->heap + ((c - 2) << v->cshift);
}

static ofs_u32 clusters_for(const struct ofs_fat *v, ofs_u32 bytes)
{
    return bytes ? ((bytes - 1) >> CB(v)) + 1 : 0;
}

/* the sector holding byte off of a file or directory */
static int file_sector(struct ofs_fat *v, struct ofs_fat_file *f, ofs_u32 off, ofs_u32 *lba)
{
    ofs_u32 c;
    int r;
    if (f->root) {
        if (off >= (ofs_u32)v->root_entries * 32) return OFS_FAT_END;
        *lba = v->root + (off >> v->sshift);
        return OFS_FAT_OK;
    }
    if ((r = walk(v, f, off >> CB(v), &c)) != 0) return r;
    *lba = cluster_lba(v, c) + ((off >> v->sshift) & ((1UL << v->cshift) - 1));
    return OFS_FAT_OK;
}

/* ---- mounting ---- */

static int pow2(ofs_u32 x) { return x && !(x & (x - 1)); }
static unsigned log2u(ofs_u32 x) { unsigned n = 0; while (x > 1) { x >>= 1; ++n; } return n; }

/* the geometry of a boot sector: 0 when it is no FAT one */
static int geometry(const ofs_u8 OFS_FAR *b, struct ofs_fat *v)
{
    ofs_u32 bps = rd16(b + 11), spc = b[13], res = rd16(b + 14), total, fatsz, rootsecs;
    if (b[0] != 0xEB && b[0] != 0xE9) return 0;
    if (!pow2(bps) || bps < 512 || bps > 4096 || !pow2(spc) || spc > 128 || !res || !b[16]) return 0;
    if (b[21] != 0xF0 && b[21] < 0xF8) return 0;
    total = rd16(b + 19) ? rd16(b + 19) : rd32(b + 32);
    fatsz = rd16(b + 22) ? rd16(b + 22) : rd32(b + 36);
    v->root_entries = rd16(b + 17);
    rootsecs = ((ofs_u32)v->root_entries * 32 + bps - 1) / bps;
    if (!total || !fatsz) return 0;
    v->sshift = (ofs_u8)log2u(bps);
    v->cshift = (ofs_u8)log2u(spc);
    v->nfats = b[16];
    v->fat = res;
    v->fat_size = fatsz;
    v->heap = res + b[16] * fatsz + rootsecs;
    if (v->heap >= total) return 0;
    v->clusters = (total - v->heap) >> v->cshift;
    /* FAT32 by its BPB, as Linux and Windows do (small FAT32 volumes
       exist); FAT12 or FAT16 by the cluster count */
    if (!rd16(b + 22)) {
        if (v->root_entries || !rd32(b + 36)) return 0;
        v->bits = 32;
    } else v->bits = v->clusters < 4085 ? 12 : 16;
    if (v->bits == 32 && v->clusters > 0x0FFFFFF5UL) return 0;
    if (v->bits == 12 ? (v->clusters + 2) * 3 / 2 > fatsz * bps
                      : v->clusters + 2 > fatsz * (bps / (v->bits / 8))) return 0;
    return v->bits;
}

int ofs_fat_probe(const ofs_u8 OFS_FAR *boot)
{
    struct ofs_fat v;
    return geometry(boot, &v);
}

int ofs_fat_mount(struct ofs_fat *v, ofs_fat_read_fn read, void *dev, ofs_lba start,
                   const struct ofs_cp *cp, ofs_u8 *buf)
{
    const ofs_u8 *b, *l;
    ofs_u32 e1;
    int r;
    memset(v, 0, sizeof(*v));
    v->read = read; v->dev = dev; v->start = start; v->cp = cp; v->buf = buf;
    v->buf_lba = NOBUF;
    v->free = NOBUF;
    if (read(dev, start, buf)) return OFS_FAT_EIO;
    if (rd16(buf + 11) > OFS_FAT_MAXSECT && pow2(rd16(buf + 11))) return OFS_FAT_ESIZE;
    if (!geometry(buf, v)) return OFS_FAT_EBAD;
    v->buf_lba = 0;
    b = buf;
    v->mirror = 1;
    if (v->bits == 32) {
        if (b[40] & 0x80) { v->mirror = 0; v->active = (ofs_u8)(b[40] & 15); }
        if (v->active >= v->nfats) return OFS_FAT_EBAD;
        v->root = rd32(b + 44);
        if (!valid(v, v->root)) return OFS_FAT_EBAD;
        v->fsinfo = rd16(b + 48);
        l = b + 64;
    } else {
        v->root = v->fat + v->nfats * v->fat_size;
        l = b + 36;
    }
    if (l[2] == 0x29) { v->serial = rd32(l + 3); memcpy(v->label, l + 7, 11); }
    else memset(v->label, ' ', 11);
    if (v->fsinfo && v->fsinfo < v->fat) {
        if (!(b = sector(v, v->fsinfo))) return OFS_FAT_EIO;
        if (rd32(b) == 0x41615252UL && rd32(b + 484) == 0x61417272UL) {
            if (rd32(b + 488) <= v->clusters) v->free = rd32(b + 488);
            if (valid(v, rd32(b + 492))) v->hint = rd32(b + 492);
        }
    } else v->fsinfo = 0;
    if (!v->hint) v->hint = 2;
    if (v->bits != 12) {
        if ((r = fat_get(v, 1, &e1)) != 0) return r;
        if (!(e1 & (v->bits == 16 ? 0x8000UL : 0x08000000UL))) v->dirty = 2;
    }
    return OFS_FAT_OK;
}

void ofs_fat_rootdir(const struct ofs_fat *v, struct ofs_fat_file *f)
{
    memset(f, 0, sizeof(*f));
    f->dir = 1;
    if (v->bits == 32) f->first = v->root; else f->root = 1;
}

void ofs_fat_dirfile(const struct ofs_fat *v, ofs_u32 first, struct ofs_fat_file *f)
{
    if (!first) { ofs_fat_rootdir(v, f); return; }
    memset(f, 0, sizeof(*f));
    f->dir = 1;
    f->first = first;
}

/* ---- directories ---- */

static ofs_u32 first_of(const struct ofs_fat *v, const ofs_u8 *e)
{
    ofs_u32 c = rd16(e + 26);
    if (v->bits == 32) c |= (ofs_u32)rd16(e + 20) << 16;
    return c;
}

/* the 32-byte entry at pos of a directory (pointer into the buffer) */
static int dir_entry(struct ofs_fat *v, struct ofs_fat_file *d, ofs_u32 pos, const ofs_u8 **e)
{
    ofs_u32 lba;
    const ofs_u8 *s;
    int r = file_sector(v, d, pos, &lba);
    if (r) return r;
    if (!(s = sector(v, lba))) return OFS_FAT_EIO;
    *e = s + ((unsigned)pos & ((1u << v->sshift) - 1));
    return OFS_FAT_OK;
}

static int put_entry(struct ofs_fat *v, struct ofs_fat_file *d, ofs_u32 pos, const ofs_u8 *e)
{
    ofs_u32 lba;
    ofs_u8 *s;
    int r = file_sector(v, d, pos, &lba);
    if (r) return r == OFS_FAT_END ? OFS_FAT_EBAD : r;
    if (!(s = sector(v, lba))) return OFS_FAT_EIO;
    memcpy(s + ((unsigned)pos & ((1u << v->sshift) - 1)), e, 32);
    return put(v, lba);
}

static ofs_u8 lfn_sum(const char *fcb)
{
    ofs_u8 sum = 0;
    unsigned i;
    for (i = 0; i < 11; ++i) sum = (ofs_u8)(((sum & 1) << 7) + (sum >> 1) + (ofs_u8)fcb[i]);
    return sum;
}

static const ofs_u8 lfn_at[13] = { 1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30 };

static ofs_u16 oem_uni(const struct ofs_fat *v, ofs_u8 c)
{
    return c < 0x80 ? c : v->cp->uni[c - 0x80];
}

/* the display name of a short entry, with the NT case bits */
static unsigned short_name(const struct ofs_fat *v, const char *fcb, ofs_u8 ntcase, ofs_u16 *out)
{
    unsigned i, n = 0, len;
    ofs_u8 c;
    for (len = 8; len && fcb[len - 1] == ' '; --len) ;
    for (i = 0; i < len; ++i) {
        c = (ofs_u8)fcb[i];
        if (!i && c == 0x05) c = 0xE5;
        if ((ntcase & 0x08) && c >= 'A' && c <= 'Z') c = (ofs_u8)(c + 32);
        out[n++] = oem_uni(v, c);
    }
    for (len = 3; len && fcb[8 + len - 1] == ' '; --len) ;
    if (len) out[n++] = '.';
    for (i = 0; i < len; ++i) {
        c = (ofs_u8)fcb[8 + i];
        if ((ntcase & 0x10) && c >= 'A' && c <= 'Z') c = (ofs_u8)(c + 32);
        out[n++] = oem_uni(v, c);
    }
    return n;
}

int ofs_fat_dir_next(struct ofs_fat *v, struct ofs_fat_file *d, ofs_u32 *pos, struct ofs_fat_ent *ent)
{
    const ofs_u8 *e;
    unsigned seq = 0, count = 0, i, k;
    ofs_u8 sum = 0;
    ofs_u32 lfnpos = 0;
    int r;
    for (;; *pos += 32) {
        if ((r = dir_entry(v, d, *pos, &e)) != 0) return r;
        if (!e[0]) return OFS_FAT_END;
        if (e[0] == 0xE5) { seq = 0; continue; }
        if ((e[11] & 0x3F) == 0x0F) {                /* a long name part */
            k = e[0] & 0x1F;
            if (e[0] & 0x40) {
                if (!k || k > 20) { seq = 0; continue; }
                count = seq = k; sum = e[13]; lfnpos = *pos;
                memset(ent->name, 0, sizeof(ent->name));
            } else if (!seq || k != seq - 1 || e[13] != sum) { seq = 0; continue; }
            else seq = k;
            for (i = 0; i < 13; ++i)
                if ((k - 1) * 13 + i < 255) ent->name[(k - 1) * 13 + i] = rd16(e + lfn_at[i]);
                else if (rd16(e + lfn_at[i]) && rd16(e + lfn_at[i]) != 0xFFFF) seq = 0;
            continue;
        }
        if ((e[11] & OFS_FAT_VOLUME) || (e[0] == '.' && (e[1] == ' ' || (e[1] == '.' && e[2] == ' ')))) {
            seq = 0;
            continue;
        }
        memcpy(ent->fcb, e, 11);
        ent->pos = *pos;
        ent->attr = e[11];
        ent->ntcase = e[12];
        ent->ctime_cs = e[13];
        ent->ctime = rd16(e + 14);
        ent->cdate = rd16(e + 16);
        ent->adate = rd16(e + 18);
        ent->time = rd16(e + 22);
        ent->date = rd16(e + 24);
        memset(&ent->file, 0, sizeof(ent->file));
        ent->file.first = first_of(v, e);
        ent->file.size = rd32(e + 28);
        ent->file.dir = (ofs_u8)(e[11] & OFS_FAT_DIR ? 1 : 0);
        if (ent->file.dir) ent->file.size = 0;
        if (seq == 1 && sum == lfn_sum(ent->fcb)) {
            for (i = 0; i < count * 13 && i < 255 && ent->name[i]; ++i) ;
            ent->name_len = (ofs_u8)i;
            ent->lfn = lfnpos;
        } else {
            ent->name_len = (ofs_u8)short_name(v, ent->fcb, ent->ntcase, ent->name);
            ent->lfn = *pos;
        }
        *pos += 32;
        return OFS_FAT_OK;
    }
}

/* upper case for comparing names: ASCII and Latin-1 */
static ofs_u16 fold(ofs_u16 c)
{
    if ((c >= 'a' && c <= 'z') || (c >= 0xE0 && c <= 0xFE && c != 0xF7)) return (ofs_u16)(c - 32);
    if (c == 0xFF) return 0x178;
    return c;
}

static int same_name(const ofs_u16 *a, unsigned na, const ofs_u16 *b, unsigned nb)
{
    unsigned i;
    if (na != nb) return 0;
    for (i = 0; i < na; ++i) if (fold(a[i]) != fold(b[i])) return 0;
    return 1;
}

int ofs_fat_find(struct ofs_fat *v, struct ofs_fat_file *d, const ofs_u16 *name, unsigned len,
                  struct ofs_fat_ent *ent)
{
    static ofs_u16 sn[12];
    ofs_u32 pos = 0;
    unsigned n;
    int r;
    while (!(r = ofs_fat_dir_next(v, d, &pos, ent))) {
        if (same_name(ent->name, ent->name_len, name, len)) return OFS_FAT_OK;
        n = short_name(v, ent->fcb, 0, sn);
        if (same_name(sn, n, name, len)) return OFS_FAT_OK;
    }
    return r;
}

int ofs_fat_lookup(struct ofs_fat *v, struct ofs_fat_file *d, const char fcb[11], struct ofs_fat_ent *ent)
{
    ofs_u32 pos = 0;
    int r;
    while (!(r = ofs_fat_dir_next(v, d, &pos, ent)))
        if (!memcmp(ent->fcb, fcb, 11)) return OFS_FAT_OK;
    return r;
}

/* ---- reading ---- */

int ofs_fat_read(struct ofs_fat *v, struct ofs_fat_file *f, ofs_u32 off, void OFS_FAR *buf,
                  ofs_u16 len, ofs_u16 *done)
{
    ofs_u8 OFS_FAR *out = (ofs_u8 OFS_FAR *)buf;
    unsigned ssize = 1u << v->sshift, in, n;
    ofs_u32 lba;
    const ofs_u8 *s;
    int r;
    *done = 0;
    if (f->dir) return OFS_FAT_EBAD;
    if (off >= f->size) return OFS_FAT_OK;
    if (len > f->size - off) len = (ofs_u16)(f->size - off);
    while (len) {
        in = (unsigned)off & (ssize - 1);
        n = ssize - in;
        if (n > len) n = len;
        r = file_sector(v, f, off, &lba);
        if (r) return r == OFS_FAT_END ? OFS_FAT_EBAD : r;
        if (!in && n == ssize) {
            if (v->buf_lba == lba) v->buf_lba = NOBUF;
            if (v->read(v->dev, v->start + lba, out)) return OFS_FAT_EIO;
        } else {
            if (!(s = sector(v, lba))) return OFS_FAT_EIO;
            fmemcpy(out, s + in, n);
        }
        out += n; off += n; len = (ofs_u16)(len - n); *done = (ofs_u16)(*done + n);
    }
    return OFS_FAT_OK;
}

int ofs_fat_free(struct ofs_fat *v, ofs_u32 *free_clusters)
{
    ofs_u32 c, val;
    int r;
    *free_clusters = 0;
    for (c = 2; c < v->clusters + 2; ++c) {
        if ((r = fat_get(v, c, &val)) != 0) return r;
        if (!val) ++*free_clusters;
    }
    v->free = *free_clusters;
    return OFS_FAT_OK;
}

/* ---- writing ---- */

/* the dirty bit, before the first change */
static int begin(struct ofs_fat *v)
{
    ofs_u32 e1, bit = v->bits == 16 ? 0x8000UL : 0x08000000UL;
    int r;
    if (!v->write) return OFS_FAT_EWRITE;
    if (v->dirty || v->bits == 12) { if (!v->dirty) v->dirty = 1; return OFS_FAT_OK; }
    if ((r = fat_get(v, 1, &e1)) != 0) return r;
    if ((r = fat_set(v, 1, e1 & ~bit)) != 0) return r;
    v->dirty = 1;
    return OFS_FAT_OK;
}

int ofs_fat_clean(struct ofs_fat *v)
{
    ofs_u32 e1, bit = v->bits == 16 ? 0x8000UL : 0x08000000UL;
    ofs_u8 *s;
    int r;
    if (v->dirty != 1) return OFS_FAT_OK;
    if (v->fsinfo) {
        if (!(s = sector(v, v->fsinfo))) return OFS_FAT_EIO;
        if (rd32(s) == 0x41615252UL && rd32(s + 484) == 0x61417272UL) {
            wr32(s + 488, v->free);
            wr32(s + 492, v->hint);
            if ((r = put(v, v->fsinfo)) != 0) return r;
        }
    }
    if (v->bits != 12) {
        if ((r = fat_get(v, 1, &e1)) != 0) return r;
        if ((r = fat_set(v, 1, e1 | bit)) != 0) return r;
    }
    v->dirty = 0;
    return OFS_FAT_OK;
}

/* a free cluster, marked as the end of a chain */
static int alloc(struct ofs_fat *v, ofs_u32 *clu)
{
    ofs_u32 c = v->hint, n, val;
    int r;
    if (!valid(v, c)) c = 2;
    for (n = 0; n < v->clusters; ++n, ++c) {
        if (c > v->clusters + 1) c = 2;
        if ((r = fat_get(v, c, &val)) != 0) return r;
        if (!val) {
            if ((r = fat_set(v, c, eoc(v))) != 0) return r;
            if (v->free != NOBUF && v->free) --v->free;
            v->hint = c + 1 > v->clusters + 1 ? 2 : c + 1;
            *clu = c;
            return OFS_FAT_OK;
        }
    }
    v->free = 0;
    return OFS_FAT_EFULL;
}

static int free_chain(struct ofs_fat *v, ofs_u32 c)
{
    ofs_u32 next, n = 0;
    int r;
    while (valid(v, c) && n++ <= v->clusters) {
        if ((r = fat_get(v, c, &next)) != 0) return r;
        if ((r = fat_set(v, c, 0)) != 0) return r;
        if (v->free != NOBUF) ++v->free;
        c = next;
    }
    return OFS_FAT_OK;
}

/* zeros over a whole cluster */
static int zero_cluster(struct ofs_fat *v, ofs_u32 c)
{
    ofs_u32 lba = cluster_lba(v, c), i;
    int r;
    for (i = 0; i < (1UL << v->cshift); ++i) {
        memset(v->buf, 0, 1u << v->sshift);
        v->buf_lba = lba + i;
        if ((r = put(v, lba + i)) != 0) return r;
    }
    return OFS_FAT_OK;
}

/* the chain of f grown to `want` clusters; *have its length before */
static int grow(struct ofs_fat *v, struct ofs_fat_file *f, ofs_u32 want, int zero)
{
    ofs_u32 have = 0, last = 0, c, next;
    int r;
    if (f->first) {
        /* the last cluster and the length of the chain */
        r = walk(v, f, f->clu ? f->idx : 0, &last);
        if (r) return r == OFS_FAT_END ? OFS_FAT_EBAD : r;
        have = f->idx;
        for (;;) {
            if ((r = fat_get(v, last, &next)) != 0) return r;
            if (at_end(v, next)) break;
            if (!valid(v, next) || have > v->clusters) return OFS_FAT_EBAD;
            last = next; ++have;
        }
        ++have;
        f->idx = have - 1; f->clu = last;
    }
    while (have < want) {
        if ((r = alloc(v, &c)) != 0) return r;
        if (zero && (r = zero_cluster(v, c)) != 0) return r;
        if (last) { if ((r = fat_set(v, last, c)) != 0) return r; }
        else f->first = c;
        last = c;
        f->idx = have++; f->clu = c;
    }
    return OFS_FAT_OK;
}

/* the short entry of a file: first cluster and size */
static int put_size(struct ofs_fat *v, const struct ofs_fat_where *w, const struct ofs_fat_file *f)
{
    struct ofs_fat_file d;
    const ofs_u8 *e;
    int r;
    if (w->none) return OFS_FAT_OK;
    ofs_fat_dirfile(v, w->dir, &d);
    if ((r = dir_entry(v, &d, w->pos, &e)) != 0) return r == OFS_FAT_END ? OFS_FAT_EBAD : r;
    memcpy(se, e, 32);
    wr16(se + 26, (ofs_u16)f->first);
    if (v->bits == 32) wr16(se + 20, (ofs_u16)(f->first >> 16));
    if (!f->dir) wr32(se + 28, f->size);
    return put_entry(v, &d, w->pos, se);
}

/* data at off, from in or zeros; the chain must reach off + len */
static int put_data(struct ofs_fat *v, struct ofs_fat_file *f, ofs_u32 off,
                    const ofs_u8 OFS_FAR *in, ofs_u32 len)
{
    unsigned ssize = 1u << v->sshift, at, n;
    ofs_u32 lba;
    ofs_u8 *s;
    int r;
    while (len) {
        at = (unsigned)off & (ssize - 1);
        n = ssize - at;
        if (n > len) n = (unsigned)len;
        if ((r = file_sector(v, f, off, &lba)) != 0) return r == OFS_FAT_END ? OFS_FAT_EBAD : r;
        if (!at && n == ssize && in) {
            if (v->buf_lba == lba) v->buf_lba = NOBUF;
            if (v->write(v->dev, v->start + lba, in)) return OFS_FAT_EWRITE;
        } else {
            if (n == ssize) { memset(v->buf, 0, ssize); v->buf_lba = lba; s = v->buf; }
            else if (!(s = sector(v, lba))) return OFS_FAT_EIO;
            if (in) fmemcpy(s + at, in, n); else memset(s + at, 0, n);
            if ((r = put(v, lba)) != 0) return r;
        }
        if (in) in += n;
        off += n; len -= n;
    }
    return OFS_FAT_OK;
}

/* writes, growing the file; in NULL: zeros */
static int extend(struct ofs_fat *v, struct ofs_fat_file *f, const struct ofs_fat_where *w,
                  ofs_u32 off, const ofs_u8 OFS_FAR *in, ofs_u32 len, ofs_u32 *done)
{
    ofs_u32 end = off + len, avail;
    int r, full = 0;
    *done = 0;
    if (end < off) return OFS_FAT_EFULL;                   /* 4 GB */
    if ((r = begin(v)) != 0) return r;
    r = grow(v, f, clusters_for(v, end), 0);
    if (r == OFS_FAT_EFULL) {
        /* as much as there is room for */
        full = 1;
        avail = f->first ? (f->idx + 1) << CB(v) : 0;
        if (avail < end) end = avail;
    } else if (r) return r;
    if (off > f->size && end > f->size) {
        if ((r = put_data(v, f, f->size, NULL, (off < end ? off : end) - f->size)) != 0) return r;
    }
    if (end > off) {
        if ((r = put_data(v, f, off, in, end - off)) != 0) return r;
        *done = end - off;
    }
    if (end > f->size) f->size = end;
    if ((r = put_size(v, w, f)) != 0) return r;
    return full ? OFS_FAT_EFULL : OFS_FAT_OK;
}

void ofs_fat_where_of(const struct ofs_fat_file *dir, ofs_u32 pos, struct ofs_fat_where *w)
{
    w->dir = dir->root ? 0 : dir->first;
    w->pos = pos;
    w->none = 0;
}

int ofs_fat_write(struct ofs_fat *v, struct ofs_fat_file *f, const struct ofs_fat_where *w,
                   ofs_u32 off, const void OFS_FAR *buf, ofs_u16 len, ofs_u16 *done)
{
    ofs_u32 n;
    int r;
    *done = 0;
    if (f->dir) return OFS_FAT_EBAD;
    if (!len) return OFS_FAT_OK;
    r = extend(v, f, w, off, (const ofs_u8 OFS_FAR *)buf, len, &n);
    *done = (ofs_u16)n;
    return r;
}

int ofs_fat_resize(struct ofs_fat *v, struct ofs_fat_file *f, const struct ofs_fat_where *w, ofs_u32 size)
{
    ofs_u32 keep, last, rest, n;
    int r;
    if (f->dir) return OFS_FAT_EBAD;
    if (size > f->size) return extend(v, f, w, size, NULL, 0, &n);
    if (size == f->size) return OFS_FAT_OK;
    if ((r = begin(v)) != 0) return r;
    keep = clusters_for(v, size);
    rest = f->first;
    if (keep) {
        if ((r = walk(v, f, keep - 1, &last)) != 0) return r == OFS_FAT_END ? OFS_FAT_EBAD : r;
        if ((r = fat_get(v, last, &rest)) != 0) return r;
    }
    f->size = size;
    if (!keep) { f->first = 0; f->clu = 0; }
    /* the entry first, then the clusters: at worst lost ones */
    if ((r = put_size(v, w, f)) != 0) return r;
    if (keep && !at_end(v, rest)) {
        if ((r = fat_set(v, last, eoc(v))) != 0) return r;
        return free_chain(v, rest);
    }
    return keep ? OFS_FAT_OK : free_chain(v, rest);
}

/* ---- creating, removing, renaming ---- */

static int bad_char(ofs_u16 c)
{
    return c < 0x20 || c == '"' || c == '*' || c == '/' || c == ':' || c == '<' ||
        c == '>' || c == '?' || c == '\\' || c == '|';
}

/* whether no entry of directory d has the short name fcb: 1, 0, or <0 */
static int short_free(struct ofs_fat *v, struct ofs_fat_file *d, const char fcb[11])
{
    ofs_u32 pos = 0;
    int r;
    while (!(r = ofs_fat_dir_next(v, d, &pos, &tmp)))
        if (!memcmp(tmp.fcb, fcb, 11)) return 0;
    return r == OFS_FAT_END ? 1 : r;
}

/* the characters of a mangled name (Novell NSS, DOSNS_generateUniqueName) */
static const char mangle_chars[] = "123456789ABCDEFGHJKLMNPQRSTUVWXY";

/* the short name of a new entry, after the rules of NSS's DOS name space:
   the name itself (ntcase for lower case ones; no long name), its upper
   case form when only the case differs (a long name keeps the case), else
   a mangled name: the first 6 characters with ~1 ... ~4, then the first 2,
   4 characters from the long name's hash and ~1 (NSS takes random ones;
   a hash keeps images reproducible). 1 is returned when a long name is
   needed. */
static int make_short(struct ofs_fat *v, struct ofs_fat_file *d, const ofs_u16 *name, unsigned len,
                      char fcb[11], ofs_u8 *ntcase)
{
    unsigned i, dot = len, k, n;
    int lower[2] = { 0, 0 }, upper[2] = { 0, 0 }, other = 0, part = 0, r;
    ofs_u32 hash = 2166136261UL;
    char want[11];
    *ntcase = 0;
    if (!ofs_cp_basis(v->cp, name, len, fcb)) {
        for (i = len; i; --i) if (name[i - 1] == '.') { dot = i - 1; break; }
        for (i = 0; i < len; ++i) {
            if (i == dot) { part = 1; continue; }
            if (name[i] >= 0x80) other = 1;
            if (name[i] >= 'a' && name[i] <= 'z') lower[part] = 1;
            if (name[i] >= 'A' && name[i] <= 'Z') upper[part] = 1;
        }
        if (!other && !(lower[0] && upper[0]) && !(lower[1] && upper[1])) {
            if (lower[0]) *ntcase |= 0x08;
            if (lower[1]) *ntcase |= 0x10;
            return 0;
        }
        /* only the case differs: the upper case form, when it is free */
        if ((r = short_free(v, d, fcb)) != 0) return r < 0 ? r : 1;
    }
    for (k = 8; k && fcb[k - 1] == ' '; --k) ;
    /* NAME~1 ... NAME~4 */
    for (n = 1; n <= 4; ++n) {
        unsigned keep = k > 6 ? 6 : k;
        memcpy(want, fcb, 11);
        want[keep] = '~';
        want[keep + 1] = (char)('0' + n);
        for (i = keep + 2; i < 8; ++i) want[i] = ' ';
        if ((r = short_free(v, d, want)) != 0) {
            if (r < 0) return r;
            memcpy(fcb, want, 11);
            return 1;
        }
    }
    /* NAxxxx~1: xxxx from the hash, ~2 ... ~Y when they run out */
    for (i = 0; i < len; ++i) hash = (hash ^ name[i]) * 16777619UL;
    for (n = 0; n < 32 * 1024U; ++n) {
        unsigned keep = k > 2 ? 2 : k;
        ofs_u32 h = hash;
        memcpy(want, fcb, 11);
        for (i = keep; i < 8; ++i) want[i] = ' ';
        for (i = 0; i < 4; ++i) {
            want[keep + i] = mangle_chars[h & 31];
            h >>= 5;
        }
        want[keep + 4] = '~';
        want[keep + 5] = mangle_chars[n >> 10];
        if ((r = short_free(v, d, want)) != 0) {
            if (r < 0) return r;
            memcpy(fcb, want, 11);
            return 1;
        }
        hash = hash * 1103515245UL + 12345UL;   /* the next try */
        hash &= 0xffffffffUL;
    }
    return OFS_FAT_EFULL;
}

/* room for n entries in a row: *pos the first; the directory grows */
static int place(struct ofs_fat *v, struct ofs_fat_file *d, unsigned n, ofs_u32 *pos)
{
    const ofs_u8 *e;
    ofs_u32 p = 0, run = 0;
    int r;
    for (;; p += 32) {
        r = dir_entry(v, d, p, &e);
        if (r == OFS_FAT_END) break;
        if (r) return r;
        if (!e[0] || e[0] == 0xE5) {
            if (!run) *pos = p;
            if (++run == n) return OFS_FAT_OK;
        } else run = 0;
    }
    if (!run) *pos = p;
    if (d->root || *pos + 32UL * n > 0x200000UL) return OFS_FAT_EFULL;   /* 65536 entries */
    /* new zeroed clusters at the end (a directory's chain is never empty) */
    return grow(v, d, clusters_for(v, *pos + 32UL * n), 1);
}

/* writes the entry set: long name parts, then the short entry se */
static int put_set(struct ofs_fat *v, struct ofs_fat_file *d, ofs_u32 pos,
                   const ofs_u16 *name, unsigned len, unsigned parts)
{
    ofs_u8 e[32];
    ofs_u8 sum = lfn_sum((const char *)se);
    unsigned k, i, j;
    ofs_u16 c;
    int r;
    for (k = parts; k; --k, pos += 32) {
        memset(e, 0, 32);
        e[0] = (ofs_u8)(k | (k == parts ? 0x40 : 0));
        e[11] = 0x0F;
        e[13] = sum;
        for (i = 0; i < 13; ++i) {
            j = (k - 1) * 13 + i;
            c = j < len ? name[j] : j == len ? 0 : 0xFFFF;
            wr16(e + lfn_at[i], c);
        }
        if ((r = put_entry(v, d, pos, e)) != 0) return r;
    }
    return put_entry(v, d, pos, se);
}

static int check_name(const ofs_u16 *name, unsigned len)
{
    unsigned i;
    if (!len || len > 255 || name[len - 1] == '.' || name[len - 1] == ' ') return OFS_FAT_ENAME;
    for (i = 0; i < len; ++i) if (bad_char(name[i])) return OFS_FAT_ENAME;
    return OFS_FAT_OK;
}

/* a new entry set for name in d with the short entry se (name and case
   bits are filled in); *pos the short entry */
static int add(struct ofs_fat *v, struct ofs_fat_file *d, const ofs_u16 *name, unsigned len, ofs_u32 *pos)
{
    char fcb[11];
    ofs_u8 ntcase;
    unsigned parts;
    int r;
    if ((r = make_short(v, d, name, len, fcb, &ntcase)) < 0) return r;
    parts = r ? (len + 12) / 13 : 0;
    memcpy(se, fcb, 11);
    if ((ofs_u8)se[0] == 0xE5) se[0] = 0x05;
    se[12] = ntcase;
    if ((r = place(v, d, parts + 1, pos)) != 0) return r;
    if ((r = put_set(v, d, *pos, name, len, parts)) != 0) return r;
    *pos += 32UL * parts;
    return OFS_FAT_OK;
}

static int exists(struct ofs_fat *v, struct ofs_fat_file *d, const ofs_u16 *name, unsigned len)
{
    int r = ofs_fat_find(v, d, name, len, &tmp);
    return r == OFS_FAT_END ? OFS_FAT_OK : r ? r : OFS_FAT_EEXIST;
}

int ofs_fat_create(struct ofs_fat *v, struct ofs_fat_file *d, const ofs_u16 *name, unsigned len,
                    ofs_u8 attr, ofs_u16 time, ofs_u16 date, ofs_u32 *pos)
{
    ofs_u8 dot[32];
    ofs_u32 c = 0, parent = d->root ? 0 : d->first;
    struct ofs_fat_file nd;
    int r;
    if ((r = check_name(name, len)) != 0) return r;
    if ((r = exists(v, d, name, len)) != 0) return r;
    if ((r = begin(v)) != 0) return r;
    if (v->bits == 32 && parent == v->root) parent = 0;
    if (attr & OFS_FAT_DIR) {
        /* the new directory with "." and ".." */
        if ((r = alloc(v, &c)) != 0) return r;
        if ((r = zero_cluster(v, c)) != 0) return r;
        ofs_fat_dirfile(v, c, &nd);
        memset(dot, 0, 32);
        memset(dot, ' ', 11);
        dot[0] = '.';
        dot[11] = OFS_FAT_DIR;
        wr16(dot + 14, time); wr16(dot + 16, date); wr16(dot + 18, date);
        wr16(dot + 22, time); wr16(dot + 24, date);
        wr16(dot + 26, (ofs_u16)c);
        if (v->bits == 32) wr16(dot + 20, (ofs_u16)(c >> 16));
        if ((r = put_entry(v, &nd, 0, dot)) != 0) return r;
        dot[1] = '.';
        wr16(dot + 26, (ofs_u16)parent);
        wr16(dot + 20, v->bits == 32 ? (ofs_u16)(parent >> 16) : 0);
        if ((r = put_entry(v, &nd, 32, dot)) != 0) return r;
    }
    memset(se, 0, 32);
    se[11] = (ofs_u8)(attr & ~OFS_FAT_VOLUME);
    wr16(se + 14, time); wr16(se + 16, date); wr16(se + 18, date);
    wr16(se + 22, time); wr16(se + 24, date);
    wr16(se + 26, (ofs_u16)c);
    if (v->bits == 32) wr16(se + 20, (ofs_u16)(c >> 16));
    r = add(v, d, name, len, pos);
    if (r && c) free_chain(v, c);
    return r;
}

/* the entry set whose short entry is at pos, in tmp */
static int entry_at(struct ofs_fat *v, struct ofs_fat_file *d, ofs_u32 pos)
{
    ofs_u32 p = pos >= 20 * 32 + 32 ? pos - 20 * 32 : 0;
    int r;
    /* from up to 20 entries before: a long name has at most 20 parts */
    for (;;) {
        r = ofs_fat_dir_next(v, d, &p, &tmp);
        if (r) return r == OFS_FAT_END ? OFS_FAT_EBAD : r;
        if (tmp.pos == pos) return OFS_FAT_OK;
        if (tmp.pos > pos) return OFS_FAT_EBAD;
    }
}

/* marks the entries of tmp's set deleted, the short one first */
static int unmark(struct ofs_fat *v, struct ofs_fat_file *d)
{
    const ofs_u8 *e;
    ofs_u32 p = tmp.pos, end = tmp.pos;
    int r;
    for (;;) {
        if ((r = dir_entry(v, d, p, &e)) != 0) return r == OFS_FAT_END ? OFS_FAT_EBAD : r;
        memcpy(se, e, 32);
        se[0] = 0xE5;
        if ((r = put_entry(v, d, p, se)) != 0) return r;
        p = p == end ? tmp.lfn : p + 32;
        if (p == end) return OFS_FAT_OK;
    }
}

int ofs_fat_remove(struct ofs_fat *v, struct ofs_fat_file *d, ofs_u32 pos)
{
    ofs_u32 c;
    int r;
    if ((r = entry_at(v, d, pos)) != 0) return r;
    if ((r = begin(v)) != 0) return r;
    c = tmp.file.first;
    if ((r = unmark(v, d)) != 0) return r;
    return free_chain(v, c);
}

int ofs_fat_empty(struct ofs_fat *v, struct ofs_fat_file *d)
{
    ofs_u32 pos = 0;
    int r = ofs_fat_dir_next(v, d, &pos, &tmp);
    return r == OFS_FAT_END ? 1 : r ? r : 0;
}

int ofs_fat_rename(struct ofs_fat *v, struct ofs_fat_file *d, ofs_u32 pos,
                    struct ofs_fat_file *to, const ofs_u16 *name, unsigned len, ofs_u32 *newpos)
{
    static ofs_u8 old[32];
    struct ofs_fat_file moved;
    ofs_u32 parent, lfn;
    const ofs_u8 *e;
    int r, same = (d->root && to->root) || (!d->root && !to->root && d->first == to->first);
    if ((r = check_name(name, len)) != 0) return r;
    r = ofs_fat_find(v, to, name, len, &tmp);
    if (!r && !(same && tmp.pos == pos)) return OFS_FAT_EEXIST;
    if (r && r != OFS_FAT_END) return r;
    if ((r = entry_at(v, d, pos)) != 0) return r;
    lfn = tmp.lfn;
    if ((r = begin(v)) != 0) return r;
    if ((r = dir_entry(v, d, pos, &e)) != 0) return r;
    memcpy(old, e, 32);
    /* the new set first, then the old one removed: at worst both stay */
    memcpy(se, old, 32);
    if ((r = add(v, to, name, len, newpos)) != 0) return r;
    tmp.pos = pos; tmp.lfn = lfn;
    if ((r = unmark(v, d)) != 0) return r;
    if (!same) {
        if (old[11] & OFS_FAT_DIR) {
            /* ".." of a moved directory */
            parent = to->root ? 0 : to->first;
            if (v->bits == 32 && parent == v->root) parent = 0;
            ofs_fat_dirfile(v, first_of(v, old), &moved);
            if ((r = dir_entry(v, &moved, 32, &e)) != 0) return r == OFS_FAT_END ? OFS_FAT_EBAD : r;
            memcpy(se, e, 32);
            if (se[0] == '.' && se[1] == '.') {
                wr16(se + 26, (ofs_u16)parent);
                if (v->bits == 32) wr16(se + 20, (ofs_u16)(parent >> 16));
                if ((r = put_entry(v, &moved, 32, se)) != 0) return r;
            }
        }
    }
    return OFS_FAT_OK;
}

static int change(struct ofs_fat *v, struct ofs_fat_file *d, ofs_u32 pos)
{
    const ofs_u8 *e;
    int r = begin(v);
    if (r) return r;
    if ((r = dir_entry(v, d, pos, &e)) != 0) return r == OFS_FAT_END ? OFS_FAT_EBAD : r;
    memcpy(se, e, 32);
    return OFS_FAT_OK;
}

int ofs_fat_attr(struct ofs_fat *v, struct ofs_fat_file *d, ofs_u32 pos, ofs_u8 attr)
{
    int r = change(v, d, pos);
    if (r) return r;
    se[11] = (ofs_u8)((se[11] & (OFS_FAT_DIR | OFS_FAT_VOLUME)) |
                       (attr & ~(OFS_FAT_DIR | OFS_FAT_VOLUME)));
    return put_entry(v, d, pos, se);
}

int ofs_fat_time(struct ofs_fat *v, struct ofs_fat_file *d, ofs_u32 pos, unsigned which,
                  ofs_u16 time, ofs_u16 date, ofs_u8 cs10)
{
    int r = change(v, d, pos);
    if (r) return r;
    if (which == OFS_FAT_ACCESSED) wr16(se + 18, date);
    else {
        wr16(se + which, time);
        wr16(se + which + 2, date);
        if (which == OFS_FAT_CREATED) se[13] = cs10;
    }
    return put_entry(v, d, pos, se);
}
