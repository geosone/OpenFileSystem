/* SPDX-License-Identifier: GPL-2.0-or-later */
/* One interface for every file system: see include/ofs/vol.h.  Each core
   gets a table of operations over its own functions; what all of them need
   around that (the name compare, reading and writing in pieces, the
   label) is here once. */
#include <string.h>
#include "ofs/vol.h"

struct ofs_fs {
    /* NULL: no boot sector, mount itself looks (tried after the others) */
    int (*probe)(const ofs_u8 OFS_FAR *);
    int (*mount)(struct ofs_vol *, ofs_lba start, const struct ofs_cp *, ofs_u8 *buf);
    void (*root)(struct ofs_vol *, struct ofs_node *);
    int (*next)(struct ofs_vol *, struct ofs_node *dir, ofs_u32 *pos, struct ofs_dirent *);
    /* NULL: ofs_lookup goes through next */
    int (*lookup)(struct ofs_vol *, struct ofs_node *dir, const ofs_u16 *, unsigned, struct ofs_dirent *);
    int (*read)(struct ofs_vol *, struct ofs_node *, ofs_fsize off, void OFS_FAR *, ofs_u16, ofs_u16 *);
    int (*statfs)(struct ofs_vol *, ofs_u32 *free_clusters, ofs_u32 *cluster_bytes, ofs_u32 *clusters);
#ifndef OFS_READONLY
    /* NULL: the file system cannot write */
    int (*write)(struct ofs_vol *, struct ofs_node *, ofs_fsize off, const void OFS_FAR *, ofs_u16, ofs_u16 *);
    int (*resize)(struct ofs_vol *, struct ofs_node *, ofs_fsize);
    int (*create)(struct ofs_vol *, struct ofs_node *dir, const ofs_u16 *, unsigned, int is_dir, ofs_u32 *pos);
    int (*empty)(struct ofs_vol *, struct ofs_node *dir);
    int (*remove)(struct ofs_vol *, struct ofs_node *dir, ofs_u32 pos);
    int (*rename)(struct ofs_vol *, struct ofs_node *dir, ofs_u32 pos, struct ofs_node *to,
                  const ofs_u16 *, unsigned);
    int (*sync)(struct ofs_vol *);
    ofs_fsize max_size;           /* the largest file */
#endif
};

/* ---- shared ---- */

static unsigned utf8(ofs_u16 c, char *out)
{
    if (c < 0x80) { out[0] = (char)c; return 1; }
    if (c < 0x800) { out[0] = (char)(0xC0 | c >> 6); out[1] = (char)(0x80 | (c & 63)); return 2; }
    out[0] = (char)(0xE0 | c >> 12); out[1] = (char)(0x80 | (c >> 6 & 63)); out[2] = (char)(0x80 | (c & 63));
    return 3;
}

static void label16(struct ofs_vol *v, const ofs_u16 *l, unsigned n)
{
    unsigned i, k = 0;
    for (i = 0; i < n && k + 3 < sizeof(v->label); ++i) k += utf8(l[i], v->label + k);
    v->label[k] = 0;
}

static int same(const ofs_u16 *a, unsigned alen, const ofs_u16 *b, unsigned blen)
{
    unsigned i;
    if (alen != blen) return 0;
    for (i = 0; i < alen; ++i)
        if (a[i] != b[i] && ofs_unicode_upper(a[i]) != ofs_unicode_upper(b[i])) return 0;
    return 1;
}

#ifndef OFS_READONLY
static int now(struct ofs_vol *v, ofs_u16 *t, ofs_u16 *d)
{
    *t = *d = 0;
    if (v->host.now) v->host.now(v->dev, t, d);
    return 0;
}
#endif

/* ---- FAT ---- */
#ifndef OFS_NO_FAT
static int fat_mount(struct ofs_vol *v, ofs_lba start, const struct ofs_cp *cp, ofs_u8 *buf)
{
    struct ofs_fat *f = &v->u.fat;
    unsigned i, n = 11, k = 0;
    int r = ofs_fat_mount(f, (ofs_fat_read_fn)v->host.read, v->dev, start, cp, buf);
    if (r) return r;
    v->type = f->bits == 12 ? "FAT12" : f->bits == 16 ? "FAT16" : "FAT32";
    v->dirty = f->dirty == 2;
    while (n && f->label[n - 1] == ' ') --n;
    for (i = 0; i < n; ++i) {
        ofs_u8 c = (ofs_u8)f->label[i];
        k += utf8(c < 0x80 || !cp ? c : cp->uni[c - 0x80], v->label + k);
    }
    v->label[k] = 0;
#ifndef OFS_READONLY
    f->write = (ofs_fat_write_fn)v->host.write;
#endif
    return 0;
}

static void fat_root(struct ofs_vol *v, struct ofs_node *n)
{
    ofs_fat_rootdir(&v->u.fat, &n->u.fat.f);
    n->u.fat.w.none = 1;
}

static void fat_ent(struct ofs_vol *v, struct ofs_node *dir, const struct ofs_fat_ent *e, struct ofs_dirent *de)
{
    struct ofs_node *n = &de->node;
    memset(n, 0, sizeof(*n));
    de->name_len = e->name_len;
    memcpy(de->name, e->name, e->name_len * sizeof(ofs_u16));
    de->attr = n->attr = e->attr;
    de->time = e->time;
    de->date = e->date;
    n->dir = (e->attr & OFS_FAT_DIR) != 0;
    n->size = e->file.size;
    if (n->dir) ofs_fat_dirfile(&v->u.fat, e->file.first, &n->u.fat.f);
    else n->u.fat.f = e->file;
    ofs_fat_where_of(&dir->u.fat.f, e->pos, &n->u.fat.w);
}

static struct ofs_fat_ent fent;     /* scratch: calls come one at a time */

static int fat_next(struct ofs_vol *v, struct ofs_node *dir, ofs_u32 *pos, struct ofs_dirent *de)
{
    int r = ofs_fat_dir_next(&v->u.fat, &dir->u.fat.f, pos, &fent);
    if (r) return r;
    fat_ent(v, dir, &fent, de);
    return 0;
}

static int fat_lookup(struct ofs_vol *v, struct ofs_node *dir, const ofs_u16 *name, unsigned len,
                      struct ofs_dirent *de)
{
    int r = ofs_fat_find(&v->u.fat, &dir->u.fat.f, name, len, &fent);
    if (r) return r == OFS_FAT_END ? OFS_ENOENT : r;
    fat_ent(v, dir, &fent, de);
    return 0;
}

static int fat_read(struct ofs_vol *v, struct ofs_node *n, ofs_fsize off, void OFS_FAR *buf, ofs_u16 len,
                    ofs_u16 *done)
{
    return ofs_fat_read(&v->u.fat, &n->u.fat.f, (ofs_u32)off, buf, len, done);
}

static int fat_statfs(struct ofs_vol *v, ofs_u32 *fr, ofs_u32 *cb, ofs_u32 *cl)
{
    *cb = (ofs_u32)1 << (v->u.fat.sshift + v->u.fat.cshift);
    *cl = v->u.fat.clusters;
    return ofs_fat_free(&v->u.fat, fr);
}

#ifndef OFS_READONLY
static int fat_write(struct ofs_vol *v, struct ofs_node *n, ofs_fsize off, const void OFS_FAR *buf,
                     ofs_u16 len, ofs_u16 *done)
{
    int r = ofs_fat_write(&v->u.fat, &n->u.fat.f, &n->u.fat.w, (ofs_u32)off, buf, len, done);
    n->size = n->u.fat.f.size;
    return r;
}

static int fat_resize(struct ofs_vol *v, struct ofs_node *n, ofs_fsize size)
{
    int r = ofs_fat_resize(&v->u.fat, &n->u.fat.f, &n->u.fat.w, (ofs_u32)size);
    n->size = n->u.fat.f.size;
    return r;
}

static int fat_create(struct ofs_vol *v, struct ofs_node *dir, const ofs_u16 *name, unsigned len, int is_dir,
                      ofs_u32 *pos)
{
    ofs_u16 t, d;
    now(v, &t, &d);
    return ofs_fat_create(&v->u.fat, &dir->u.fat.f, name, len, is_dir ? OFS_FAT_DIR : OFS_FAT_ARCH, t, d, pos);
}

static int fat_empty(struct ofs_vol *v, struct ofs_node *dir) { return ofs_fat_empty(&v->u.fat, &dir->u.fat.f); }

static int fat_remove(struct ofs_vol *v, struct ofs_node *dir, ofs_u32 pos)
{
    return ofs_fat_remove(&v->u.fat, &dir->u.fat.f, pos);
}

static int fat_rename(struct ofs_vol *v, struct ofs_node *dir, ofs_u32 pos, struct ofs_node *to,
                      const ofs_u16 *name, unsigned len)
{
    ofs_u32 newpos;
    return ofs_fat_rename(&v->u.fat, &dir->u.fat.f, pos, &to->u.fat.f, name, len, &newpos);
}

static int fat_sync(struct ofs_vol *v) { return ofs_fat_clean(&v->u.fat); }
#endif

static const struct ofs_fs fs_fat = {
    ofs_fat_probe, fat_mount, fat_root, fat_next, fat_lookup, fat_read, fat_statfs
#ifndef OFS_READONLY
    , fat_write, fat_resize, fat_create, fat_empty, fat_remove, fat_rename, fat_sync, 0xFFFFFFFFUL
#endif
};
#endif

/* ---- exFAT ---- */
#ifndef OFS_NO_EXFAT
static int ex_mount(struct ofs_vol *v, ofs_lba start, const struct ofs_cp *cp, ofs_u8 *buf)
{
    struct ofs_exfat *x = &v->u.ex;
    int r = ofs_exfat_mount(x, (ofs_exfat_read_fn)v->host.read, v->dev, start, cp, buf);
    if (r) return r;
    v->type = "exFAT";
    v->dirty = x->dirty == 2 || (x->flags & 4);
    label16(v, x->label, x->label_len);
#ifndef OFS_READONLY
    x->write = (ofs_exfat_write_fn)v->host.write;
#endif
    return 0;
}

static void ex_root(struct ofs_vol *v, struct ofs_node *n)
{
    ofs_exfat_rootdir(&v->u.ex, &n->u.ex.f);
    n->u.ex.w.none = 1;
}

static struct ofs_exfat_ent xent;

static int ex_next(struct ofs_vol *v, struct ofs_node *dir, ofs_u32 *pos, struct ofs_dirent *de)
{
    struct ofs_node *n = &de->node;
    int r = ofs_exfat_dir_next(&v->u.ex, &dir->u.ex.f, pos, &xent);
    if (r) return r;
    memset(n, 0, sizeof(*n));
    de->name_len = xent.name_len;
    memcpy(de->name, xent.name, xent.name_len * sizeof(ofs_u16));
    de->attr = n->attr = (ofs_u8)xent.attr;
    de->time = xent.time;
    de->date = xent.date;
    n->dir = (xent.attr & OFS_EXFAT_DIR) != 0;
    n->size = xent.file.size;
    n->u.ex.f = xent.file;
    ofs_exfat_where_of(&dir->u.ex.f, xent.pos, &n->u.ex.w);
    return 0;
}

static int ex_read(struct ofs_vol *v, struct ofs_node *n, ofs_fsize off, void OFS_FAR *buf, ofs_u16 len,
                   ofs_u16 *done)
{
    return ofs_exfat_read(&v->u.ex, &n->u.ex.f, off, buf, len, done);
}

static int ex_statfs(struct ofs_vol *v, ofs_u32 *fr, ofs_u32 *cb, ofs_u32 *cl)
{
    *cb = (ofs_u32)1 << (v->u.ex.sshift + v->u.ex.cshift);
    *cl = v->u.ex.clusters;
    return ofs_exfat_free(&v->u.ex, fr);
}

#ifndef OFS_READONLY
static int ex_write(struct ofs_vol *v, struct ofs_node *n, ofs_fsize off, const void OFS_FAR *buf,
                    ofs_u16 len, ofs_u16 *done)
{
    int r = ofs_exfat_write(&v->u.ex, &n->u.ex.f, &n->u.ex.w, off, buf, len, done);
    n->size = n->u.ex.f.size;
    return r;
}

static int ex_resize(struct ofs_vol *v, struct ofs_node *n, ofs_fsize size)
{
    int r = ofs_exfat_resize(&v->u.ex, &n->u.ex.f, &n->u.ex.w, size);
    n->size = n->u.ex.f.size;
    return r;
}

static int ex_create(struct ofs_vol *v, struct ofs_node *dir, const ofs_u16 *name, unsigned len, int is_dir,
                     ofs_u32 *pos)
{
    ofs_u16 t, d;
    now(v, &t, &d);
    return ofs_exfat_create(&v->u.ex, &dir->u.ex.f, &dir->u.ex.w, name, len,
                            is_dir ? OFS_EXFAT_DIR : OFS_EXFAT_ARCH, t, d, pos);
}

static int ex_empty(struct ofs_vol *v, struct ofs_node *dir) { return ofs_exfat_empty(&v->u.ex, &dir->u.ex.f); }

static int ex_remove(struct ofs_vol *v, struct ofs_node *dir, ofs_u32 pos)
{
    return ofs_exfat_remove(&v->u.ex, &dir->u.ex.f, pos);
}

static int ex_rename(struct ofs_vol *v, struct ofs_node *dir, ofs_u32 pos, struct ofs_node *to,
                     const ofs_u16 *name, unsigned len)
{
    ofs_u32 newpos;
    return ofs_exfat_rename(&v->u.ex, &dir->u.ex.f, pos, &to->u.ex.f, &to->u.ex.w, name, len, &newpos);
}

static int ex_sync(struct ofs_vol *v) { return ofs_exfat_clean(&v->u.ex); }
#endif

static const struct ofs_fs fs_exfat = {
    ofs_exfat_probe, ex_mount, ex_root, ex_next, NULL, ex_read, ex_statfs
#ifndef OFS_READONLY
    , ex_write, ex_resize, ex_create, ex_empty, ex_remove, ex_rename, ex_sync,
#ifdef OFS_NARROW
    0xFFFFFFFFUL
#else
    ~(ofs_fsize)0
#endif
#endif
};
#endif

/* ---- NTFS (read-only) ---- */
#ifndef OFS_NO_NTFS
static int nt_mount(struct ofs_vol *v, ofs_lba start, const struct ofs_cp *cp, ofs_u8 *buf)
{
    struct ofs_ntfs *x = &v->u.ntfs;
    int r = ofs_ntfs_mount(x, (ofs_ntfs_read_fn)v->host.read, v->dev, start, v->host.sshift, cp, buf);
    if (r) return r == OFS_NTFS_ECOMP ? OFS_ENOTSUP : r;
    x->tz = v->host.tz;
    v->type = "NTFS";
    v->dirty = x->dirty;
    label16(v, x->label, x->label_len);
    return 0;
}

static void nt_root(struct ofs_vol *v, struct ofs_node *n) { ofs_ntfs_rootdir(&v->u.ntfs, &n->u.ntfs); }

static struct ofs_ntfs_ent nent;

static void nt_ent(struct ofs_dirent *de)
{
    struct ofs_node *n = &de->node;
    memset(n, 0, sizeof(*n));
    de->name_len = nent.name_len;
    memcpy(de->name, nent.name, nent.name_len * sizeof(ofs_u16));
    de->attr = n->attr = (ofs_u8)nent.attr;
    de->time = nent.time;
    de->date = nent.date;
    n->dir = (nent.attr & OFS_NTFS_DIR) != 0;
    n->size = nent.file.size;
    n->u.ntfs = nent.file;
}

static int nt_err(int r) { return r == OFS_NTFS_ECOMP ? OFS_ENOTSUP : r; }

static int nt_next(struct ofs_vol *v, struct ofs_node *dir, ofs_u32 *pos, struct ofs_dirent *de)
{
    int r = ofs_ntfs_dir_next(&v->u.ntfs, &dir->u.ntfs, pos, &nent);
    if (r) return nt_err(r);
    nt_ent(de);
    return 0;
}

/* names only (no MFT record per entry), the hit's record afterwards */
static int nt_lookup(struct ofs_vol *v, struct ofs_node *dir, const ofs_u16 *name, unsigned len,
                     struct ofs_dirent *de)
{
    ofs_u32 pos = 0;
    int r;
    while (!(r = ofs_ntfs_dir_name(&v->u.ntfs, &dir->u.ntfs, &pos, &nent)))
        if (same(nent.name, nent.name_len, name, len)) {
            if ((r = ofs_ntfs_stat(&v->u.ntfs, &nent))) return nt_err(r);
            nt_ent(de);
            return 0;
        }
    return r == OFS_NTFS_END ? OFS_ENOENT : nt_err(r);
}

static int nt_read(struct ofs_vol *v, struct ofs_node *n, ofs_fsize off, void OFS_FAR *buf, ofs_u16 len,
                   ofs_u16 *done)
{
    return nt_err(ofs_ntfs_read(&v->u.ntfs, &n->u.ntfs, off, buf, len, done));
}

static int nt_statfs(struct ofs_vol *v, ofs_u32 *fr, ofs_u32 *cb, ofs_u32 *cl)
{
    *cb = (ofs_u32)512 << v->u.ntfs.cshift;
    *cl = v->u.ntfs.clusters;
    return ofs_ntfs_free(&v->u.ntfs, fr);
}

static const struct ofs_fs fs_ntfs = {
    ofs_ntfs_probe, nt_mount, nt_root, nt_next, nt_lookup, nt_read, nt_statfs
#ifndef OFS_READONLY
    , NULL, NULL, NULL, NULL, NULL, NULL, NULL, 0
#endif
};
#endif

/* ---- ISO 9660 (read-only) ---- */
#ifndef OFS_NO_ISO
static int iso_mount(struct ofs_vol *v, ofs_lba start, const struct ofs_cp *cp, ofs_u8 *buf)
{
    struct ofs_iso *x = &v->u.iso;
    int r = ofs_iso_mount(x, (ofs_iso_read_fn)v->host.read, v->dev, start, v->host.sshift ? v->host.sshift : 9,
                          cp, buf);
    if (r) return r == OFS_ISO_EUNSUP ? OFS_ENOTSUP : r;
    x->tz = v->host.tz;
    v->type = "ISO9660";
    label16(v, x->label, x->label_len);
    return 0;
}

static void iso_root(struct ofs_vol *v, struct ofs_node *n) { ofs_iso_rootdir(&v->u.iso, &n->u.iso); }

static struct ofs_iso_ent ient;

static int iso_next(struct ofs_vol *v, struct ofs_node *dir, ofs_u32 *pos, struct ofs_dirent *de)
{
    struct ofs_node *n = &de->node;
    int r = ofs_iso_dir_next(&v->u.iso, &dir->u.iso, pos, &ient);
    if (r) return r == OFS_ISO_EUNSUP ? OFS_ENOTSUP : r;
    memset(n, 0, sizeof(*n));
    de->name_len = ient.name_len;
    memcpy(de->name, ient.name, ient.name_len * sizeof(ofs_u16));
    de->attr = n->attr = ient.attr;
    de->time = ient.time;
    de->date = ient.date;
    n->dir = ient.file.dir;
    n->size = ient.file.size;
    n->u.iso = ient.file;
    return 0;
}

static int iso_read(struct ofs_vol *v, struct ofs_node *n, ofs_fsize off, void OFS_FAR *buf, ofs_u16 len,
                    ofs_u16 *done)
{
    int r = ofs_iso_read(&v->u.iso, &n->u.iso, off, buf, len, done);
    return r == OFS_ISO_EUNSUP ? OFS_ENOTSUP : r;
}

static int iso_statfs(struct ofs_vol *v, ofs_u32 *fr, ofs_u32 *cb, ofs_u32 *cl)
{
    *fr = 0;
    *cb = OFS_ISO_BLOCK;
    *cl = v->u.iso.blocks;
    return 0;
}

static const struct ofs_fs fs_iso = {
    NULL, iso_mount, iso_root, iso_next, NULL, iso_read, iso_statfs
#ifndef OFS_READONLY
    , NULL, NULL, NULL, NULL, NULL, NULL, NULL, 0
#endif
};
#endif

/* ---- UDF (read-only) ---- */
#ifndef OFS_NO_UDF
static int udf_err(int r) { return r == OFS_UDF_EUNSUP ? OFS_ENOTSUP : r; }

static int udf_mount(struct ofs_vol *v, ofs_lba start, const struct ofs_cp *cp, ofs_u8 *buf)
{
    struct ofs_udf *x = &v->u.udf;
    int r = ofs_udf_mount(x, (ofs_udf_read_fn)v->host.read, v->dev, start, v->host.sshift ? v->host.sshift : 9,
                          cp, buf);
    if (r) return udf_err(r);
    x->tz = v->host.tz;
    v->type = "UDF";
    label16(v, x->label, x->label_len);
    return 0;
}

static void udf_root(struct ofs_vol *v, struct ofs_node *n) { ofs_udf_rootdir(&v->u.udf, &n->u.udf); }

static struct ofs_udf_ent uent;

static int udf_next(struct ofs_vol *v, struct ofs_node *dir, ofs_u32 *pos, struct ofs_dirent *de)
{
    struct ofs_node *n = &de->node;
    int r = ofs_udf_dir_next(&v->u.udf, &dir->u.udf, pos, &uent);
    if (r) return udf_err(r);
    memset(n, 0, sizeof(*n));
    de->name_len = uent.name_len;
    memcpy(de->name, uent.name, uent.name_len * sizeof(ofs_u16));
    de->attr = n->attr = uent.attr;
    de->time = uent.time;
    de->date = uent.date;
    n->dir = uent.file.dir;
    n->size = uent.file.size;
    n->u.udf = uent.file;
    return 0;
}

static int udf_read(struct ofs_vol *v, struct ofs_node *n, ofs_fsize off, void OFS_FAR *buf, ofs_u16 len,
                    ofs_u16 *done)
{
    return udf_err(ofs_udf_read(&v->u.udf, &n->u.udf, off, buf, len, done));
}

static int udf_statfs(struct ofs_vol *v, ofs_u32 *fr, ofs_u32 *cb, ofs_u32 *cl)
{
    *fr = 0;
    *cb = (ofs_u32)1 << v->u.udf.bshift;
    *cl = v->u.udf.blocks;
    return 0;
}

static const struct ofs_fs fs_udf = {
    NULL, udf_mount, udf_root, udf_next, NULL, udf_read, udf_statfs
#ifndef OFS_READONLY
    , NULL, NULL, NULL, NULL, NULL, NULL, NULL, 0
#endif
};
#endif

/* exFAT and NTFS before FAT: their boot sectors are not FAT's, but FAT's
   probe is the most forgiving */
static const struct ofs_fs *const all[] = {
#ifndef OFS_NO_EXFAT
    &fs_exfat,
#endif
#ifndef OFS_NO_NTFS
    &fs_ntfs,
#endif
#ifndef OFS_NO_FAT
    &fs_fat,
#endif
#ifndef OFS_NO_UDF
    &fs_udf,                      /* before ISO 9660: a UDF bridge disc is read as UDF */
#endif
#ifndef OFS_NO_ISO
    &fs_iso,
#endif
    NULL
};

/* ---- the interface ---- */

int ofs_probe(const ofs_u8 OFS_FAR *s)
{
    unsigned i;
    for (i = 0; all[i]; ++i)
        if (all[i]->probe && all[i]->probe(s)) return 1;
    return 0;
}

int ofs_mount(struct ofs_vol *v, const struct ofs_host *host, void *dev, ofs_lba start,
              const struct ofs_cp *cp, ofs_u8 *buf)
{
    unsigned i;
    int r;
    memset(v, 0, sizeof(*v));
    v->host = *host;
    v->dev = dev;
    if (host->read(dev, start, buf)) return OFS_EIO;
    for (i = 0; all[i]; ++i)
        if (all[i]->probe && all[i]->probe(buf)) break;
    if (all[i]) {
        v->fs = all[i];
        if ((r = v->fs->mount(v, start, cp, buf))) {
            v->fs = NULL;
            return r;
        }
    } else {
        /* the cores without a boot sector look themselves */
        r = OFS_EBAD;
        for (i = 0; all[i]; ++i) {
            if (all[i]->probe) continue;
            v->fs = all[i];
            if (!(r = v->fs->mount(v, start, cp, buf))) break;
            v->fs = NULL;
            if (r != OFS_EBAD) return r;
        }
        if (!v->fs) return r;
    }
#ifndef OFS_READONLY
    if (host->write && v->fs->write) v->caps |= OFS_CAP_WRITE;
#endif
    return 0;
}

int ofs_root(struct ofs_vol *v, struct ofs_node *n)
{
    memset(n, 0, sizeof(*n));
    n->dir = 1;
    n->attr = OFS_ATTR_DIR;
    v->fs->root(v, n);
    return 0;
}

int ofs_next(struct ofs_vol *v, struct ofs_node *dir, ofs_u32 *pos, struct ofs_dirent *de)
{
    if (!dir->dir) return OFS_ENOTDIR;
    return v->fs->next(v, dir, pos, de);
}

int ofs_lookup(struct ofs_vol *v, struct ofs_node *dir, const ofs_u16 *name, unsigned len, struct ofs_dirent *de)
{
    ofs_u32 pos = 0;
    int r;
    if (!dir->dir) return OFS_ENOTDIR;
    if (v->fs->lookup) return v->fs->lookup(v, dir, name, len, de);
    while (!(r = v->fs->next(v, dir, &pos, de)))
        if (same(de->name, de->name_len, name, len)) return 0;
    return r == OFS_END ? OFS_ENOENT : r;
}

int ofs_read(struct ofs_vol *v, struct ofs_node *n, ofs_fsize off, void OFS_FAR *buf, ofs_u32 len, ofs_u32 *done)
{
    ofs_u8 OFS_FAR *p = (ofs_u8 OFS_FAR *)buf;
    ofs_u16 got;
    int r;
    *done = 0;
    if (n->dir) return OFS_EISDIR;
    while (len && off < n->size) {
        ofs_u16 chunk = len > 0xFE00UL ? 0xFE00 : (ofs_u16)len;
        if ((r = v->fs->read(v, n, off, p, chunk, &got))) return r;
        if (!got) break;
        p += got;
        off += got;
        len -= got;
        *done += got;
    }
    return 0;
}

int ofs_statfs(struct ofs_vol *v, ofs_u32 *free_clusters, ofs_u32 *cluster_bytes, ofs_u32 *clusters)
{
    return v->fs->statfs(v, free_clusters, cluster_bytes, clusters);
}

#ifndef OFS_READONLY
#define WRITABLE(v) if (!((v)->caps & OFS_CAP_WRITE)) return OFS_EROFS

int ofs_write(struct ofs_vol *v, struct ofs_node *n, ofs_fsize off, const void OFS_FAR *buf, ofs_u32 len)
{
    const ofs_u8 OFS_FAR *p = (const ofs_u8 OFS_FAR *)buf;
    ofs_u16 got;
    int r;
    WRITABLE(v);
    if (n->dir) return OFS_EISDIR;
    if (len && (off > v->fs->max_size || len - 1 > v->fs->max_size - off)) return OFS_EFULL;
    while (len) {
        ofs_u16 chunk = len > 0xFE00UL ? 0xFE00 : (ofs_u16)len;
        got = 0;
        r = v->fs->write(v, n, off, p, chunk, &got);
        if (r) return r;
        if (!got) return OFS_EFULL;
        p += got;
        off += got;
        len -= got;
    }
    return 0;
}

int ofs_resize(struct ofs_vol *v, struct ofs_node *n, ofs_fsize size)
{
    WRITABLE(v);
    if (n->dir) return OFS_EISDIR;
    if (size > v->fs->max_size) return OFS_EFULL;
    return v->fs->resize(v, n, size);
}

int ofs_create(struct ofs_vol *v, struct ofs_node *dir, const ofs_u16 *name, unsigned len, int is_dir,
               struct ofs_dirent *out)
{
    ofs_u32 pos;
    int r;
    WRITABLE(v);
    if (!dir->dir) return OFS_ENOTDIR;
    if (!len || len > 255) return OFS_ENAME;
    r = ofs_lookup(v, dir, name, len, out);
    if (!r) return OFS_EEXIST;
    if (r != OFS_ENOENT) return r;
    if ((r = v->fs->create(v, dir, name, len, is_dir, &pos))) return r;
    r = v->fs->next(v, dir, &pos, out);
    return r == OFS_END ? OFS_EBAD : r;
}

/* where the entry of a node is, as the core's remove and rename want it */
static int entry_pos(struct ofs_vol *v, const struct ofs_node *n, ofs_u32 *pos)
{
#ifndef OFS_NO_FAT
    if (v->fs == &fs_fat) { *pos = n->u.fat.w.pos; return 0; }
#endif
#ifndef OFS_NO_EXFAT
    if (v->fs == &fs_exfat) { *pos = n->u.ex.w.pos; return 0; }
#endif
    return OFS_EROFS;
}

int ofs_remove(struct ofs_vol *v, struct ofs_node *dir, const ofs_u16 *name, unsigned len)
{
    static struct ofs_dirent de;
    ofs_u32 pos;
    int r;
    WRITABLE(v);
    if ((r = ofs_lookup(v, dir, name, len, &de))) return r;
    if ((r = entry_pos(v, &de.node, &pos))) return r;
    if (de.node.dir) {
        r = v->fs->empty(v, &de.node);
        if (r < 0) return r;
        if (!r) return OFS_ENOTEMPTY;
    }
    return v->fs->remove(v, dir, pos);
}

int ofs_rename(struct ofs_vol *v, struct ofs_node *dir, const ofs_u16 *name, unsigned len,
               struct ofs_node *to, const ofs_u16 *to_name, unsigned to_len)
{
    static struct ofs_dirent de;
    ofs_u32 pos;
    int r;
    WRITABLE(v);
    if (!dir->dir || !to->dir) return OFS_ENOTDIR;
    if (!to_len || to_len > 255) return OFS_ENAME;
    r = ofs_lookup(v, to, to_name, to_len, &de);
    if (!r) return OFS_EEXIST;
    if (r != OFS_ENOENT) return r;
    if ((r = ofs_lookup(v, dir, name, len, &de))) return r;
    if ((r = entry_pos(v, &de.node, &pos))) return r;
    return v->fs->rename(v, dir, pos, to, to_name, to_len);
}

int ofs_sync(struct ofs_vol *v)
{
    if (!(v->caps & OFS_CAP_WRITE)) return 0;
    return v->fs->sync(v);
}
#endif
