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
    /* the attributes (as ofs_setattr) and a time (OFS_TIME_*) of the entry
       at pos in dir, which is n; NULL: not this file system */
    int (*setattr)(struct ofs_vol *, struct ofs_node *dir, ofs_u32 pos, struct ofs_node *n, ofs_u8 attr);
    int (*settime)(struct ofs_vol *, struct ofs_node *dir, ofs_u32 pos, struct ofs_node *n, unsigned which,
                   ofs_u16 time, ofs_u16 date, ofs_u8 cs10);
#endif
    /* extended attributes of the entry at pos in dir, which is n: OFS_EA_*
       or EA_GET, EA_LIST; NULL: none */
    int (*ea)(struct ofs_vol *, struct ofs_node *dir, ofs_u32 pos, struct ofs_node *n, int op, const char *name,
              void OFS_FAR *buf, ofs_u16 size, ofs_u16 *len);
#ifndef OFS_READONLY
    /* FAT32.IFS's EA mark of the entry at pos (FAT32, exFAT); NULL: none */
    int (*eamark)(struct ofs_vol *, struct ofs_node *dir, ofs_u32 pos, ofs_u8 mark);
#endif
};
#define EA_GET  8
#define EA_LIST 9

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

#if !defined(OFS_READONLY) && (!defined(OFS_NO_FAT) || !defined(OFS_NO_EXFAT))
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
    de->eas = e->eas;
    de->time = e->time;
    de->date = e->date;
    de->ctime = e->ctime; de->cdate = e->cdate; de->ctime_cs = e->ctime_cs;
    de->atime = 0; de->adate = e->adate;
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

#ifndef OFS_READONLY
static int fat_setattr(struct ofs_vol *v, struct ofs_node *dir, ofs_u32 pos, struct ofs_node *n, ofs_u8 attr)
{
    return ofs_fat_attr(&v->u.fat, &dir->u.fat.f, pos,
                        (ofs_u8)((n->attr & ~0x27) | (attr & 0x27)));
}

static int fat_settime(struct ofs_vol *v, struct ofs_node *dir, ofs_u32 pos, struct ofs_node *n, unsigned which,
                       ofs_u16 time, ofs_u16 date, ofs_u8 cs10)
{
    static const unsigned at[3] = { OFS_FAT_CREATED, OFS_FAT_MODIFIED, OFS_FAT_ACCESSED };
    (void)n;
    return ofs_fat_time(&v->u.fat, &dir->u.fat.f, pos, at[which], time, date, cs10);
}

static int fat_eamark(struct ofs_vol *v, struct ofs_node *dir, ofs_u32 pos, ofs_u8 mark)
{
    return ofs_fat_eamark(&v->u.fat, &dir->u.fat.f, pos, mark);
}
#endif

static int fat_ea(struct ofs_vol *v, struct ofs_node *dir, ofs_u32 pos, struct ofs_node *n, int op,
                  const char *name, void OFS_FAR *buf, ofs_u16 size, ofs_u16 *len)
{
    static struct ofs_fat_ent e;
#ifndef OFS_READONLY
    ofs_u16 t, d;
#endif
    int r = ofs_fat_entry_at(&v->u.fat, &dir->u.fat.f, pos, &e);
    (void)n;
    if (r) return r;
    if (op == EA_GET) return ofs_fat_ea_get(&v->u.fat, &e, name, buf, size, len);
    if (op == EA_LIST) return ofs_fat_ea_list(&v->u.fat, &e, (char OFS_FAR *)buf, size, len);
#ifdef OFS_READONLY
    return OFS_EROFS;
#else
    now(v, &t, &d);
    if (!d) d = 0x21;                  /* 1980-01-01 without a clock */
    return ofs_fat_ea_set(&v->u.fat, &dir->u.fat.f, &e, name, buf, size, op, t, d);
#endif
}

static const struct ofs_fs fs_fat = {
    ofs_fat_probe, fat_mount, fat_root, fat_next, fat_lookup, fat_read, fat_statfs
#ifndef OFS_READONLY
    , fat_write, fat_resize, fat_create, fat_empty, fat_remove, fat_rename, fat_sync, 0xFFFFFFFFUL,
    fat_setattr, fat_settime
#endif
    , fat_ea
#ifndef OFS_READONLY
    , fat_eamark
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
    de->eas = xent.eas;
    de->time = xent.time;
    de->date = xent.date;
    de->ctime = xent.ctime; de->cdate = xent.cdate; de->ctime_cs = xent.ctime_cs;
    de->atime = xent.atime; de->adate = xent.adate;
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

#ifndef OFS_READONLY
static int ex_setattr(struct ofs_vol *v, struct ofs_node *dir, ofs_u32 pos, struct ofs_node *n, ofs_u8 attr)
{
    return ofs_exfat_attr(&v->u.ex, &dir->u.ex.f, pos, (ofs_u16)((n->attr & ~0x27) | (attr & 0x27)));
}

static int ex_settime(struct ofs_vol *v, struct ofs_node *dir, ofs_u32 pos, struct ofs_node *n, unsigned which,
                      ofs_u16 time, ofs_u16 date, ofs_u8 cs10)
{
    static const unsigned at[3] = { OFS_EXFAT_CREATED, OFS_EXFAT_MODIFIED, OFS_EXFAT_ACCESSED };
    (void)n;
    return ofs_exfat_time(&v->u.ex, &dir->u.ex.f, pos, at[which], time, date, cs10);
}

static int ex_eamark(struct ofs_vol *v, struct ofs_node *dir, ofs_u32 pos, ofs_u8 mark)
{
    return ofs_exfat_eamark(&v->u.ex, &dir->u.ex.f, pos, mark);
}
#endif

static const struct ofs_fs fs_exfat = {
    ofs_exfat_probe, ex_mount, ex_root, ex_next, NULL, ex_read, ex_statfs
#ifndef OFS_READONLY
    , ex_write, ex_resize, ex_create, ex_empty, ex_remove, ex_rename, ex_sync,
#ifdef OFS_NARROW
    0xFFFFFFFFUL,
#else
    ~(ofs_fsize)0,
#endif
    ex_setattr, ex_settime
#endif
    , NULL
#ifndef OFS_READONLY
    , ex_eamark
#endif
};
#endif

/* ---- NTFS (writing in stages: overwriting, sizes) ---- */
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
#ifndef OFS_READONLY
    /* writable only when the core finds the volume safe to write (else
       x->write stays 0 and the volume read-only) */
    if (v->host.write) ofs_ntfs_set_write(x, (ofs_ntfs_write_fn)v->host.write);
#endif
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
    de->ctime = nent.ctime; de->cdate = nent.cdate; de->ctime_cs = nent.ctime_cs;
    de->atime = nent.atime; de->adate = nent.adate;
    n->dir = (nent.attr & OFS_NTFS_DIR) != 0;
    n->size = nent.file.size;
    n->u.ntfs = nent.file;
}

static int nt_err(int r)
{
    switch (r) {
    case OFS_NTFS_ECOMP: return OFS_ENOTSUP;
#ifndef OFS_READONLY
    case OFS_NTFS_EWRITE: return OFS_EWRITE;
    case OFS_NTFS_EROFS: return OFS_EROFS;
    case OFS_NTFS_ENOSPACE: return OFS_ENOTSUP;   /* not yet (a hole, a runlist too large ...) */
    case OFS_NTFS_EFULL: return OFS_EFULL;
    case OFS_NTFS_EEXIST: return OFS_EEXIST;
    case OFS_NTFS_ENAME: return OFS_ENAME;
#endif
    default: return r;                          /* EIO, EBAD and ESIZE are the same */
    }
}

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

#ifndef OFS_READONLY
static int nt_write(struct ofs_vol *v, struct ofs_node *n, ofs_fsize off, const void OFS_FAR *buf, ofs_u16 len,
                    ofs_u16 *done)
{
    ofs_u16 t = 0, d = 0;
    int r;
    if (v->host.now) v->host.now(v->dev, &t, &d);
    r = ofs_ntfs_write(&v->u.ntfs, &n->u.ntfs, off, buf, len, done, t, d);
    n->size = n->u.ntfs.size;
    return nt_err(r);
}

static int nt_resize(struct ofs_vol *v, struct ofs_node *n, ofs_fsize size)
{
    ofs_u16 t = 0, d = 0;
    int r;
    if (v->host.now) v->host.now(v->dev, &t, &d);
    r = ofs_ntfs_resize(&v->u.ntfs, &n->u.ntfs, size, t, d);
    n->size = n->u.ntfs.size;
    return nt_err(r);
}

static int nt_create(struct ofs_vol *v, struct ofs_node *dir, const ofs_u16 *name, unsigned len, int is_dir,
                     ofs_u32 *pos)
{
    ofs_u16 t = 0, d = 0;
    if (v->host.now) v->host.now(v->dev, &t, &d);
    return nt_err(ofs_ntfs_create(&v->u.ntfs, &dir->u.ntfs, name, len, is_dir, t, d, pos));
}

static int nt_empty(struct ofs_vol *v, struct ofs_node *dir) { return nt_err(ofs_ntfs_empty(&v->u.ntfs, &dir->u.ntfs)); }

static int nt_remove(struct ofs_vol *v, struct ofs_node *dir, ofs_u32 pos)
{
    ofs_u16 t = 0, d = 0;
    if (v->host.now) v->host.now(v->dev, &t, &d);
    return nt_err(ofs_ntfs_remove(&v->u.ntfs, &dir->u.ntfs, pos, t, d));
}

static int nt_rename(struct ofs_vol *v, struct ofs_node *dir, ofs_u32 pos, struct ofs_node *to,
                     const ofs_u16 *name, unsigned len)
{
    ofs_u16 t = 0, d = 0;
    ofs_u32 newpos;
    if (v->host.now) v->host.now(v->dev, &t, &d);
    return nt_err(ofs_ntfs_rename(&v->u.ntfs, &dir->u.ntfs, pos, &to->u.ntfs, name, len, t, d, &newpos));
}

static int nt_setattr(struct ofs_vol *v, struct ofs_node *dir, ofs_u32 pos, struct ofs_node *n, ofs_u8 attr)
{
    (void)dir; (void)pos;
    return nt_err(ofs_ntfs_set_attr(&v->u.ntfs, &n->u.ntfs, attr));
}

#endif

static int nt_ea(struct ofs_vol *v, struct ofs_node *dir, ofs_u32 pos, struct ofs_node *n, int op,
                 const char *name, void OFS_FAR *buf, ofs_u16 size, ofs_u16 *len)
{
    int r;
#ifndef OFS_READONLY
    ofs_u16 t = 0, d = 0;
#endif
    (void)dir; (void)pos;
    if (op == EA_GET) r = ofs_ntfs_ea_get(&v->u.ntfs, &n->u.ntfs, name, buf, size, len);
    else if (op == EA_LIST) r = ofs_ntfs_ea_list(&v->u.ntfs, &n->u.ntfs, (char OFS_FAR *)buf, size, len);
    else {
#ifdef OFS_READONLY
        return OFS_EROFS;
#else
        if (v->host.now) v->host.now(v->dev, &t, &d);
        r = ofs_ntfs_ea_set(&v->u.ntfs, &n->u.ntfs, name, buf, size, op, t, d);
        if (r == OFS_NTFS_ENOSPACE) return OFS_EFULL;   /* not in the record: not yet */
#endif
    }
    return nt_err(r);
}

#ifndef OFS_READONLY
static int nt_settime(struct ofs_vol *v, struct ofs_node *dir, ofs_u32 pos, struct ofs_node *n, unsigned which,
                      ofs_u16 time, ofs_u16 date, ofs_u8 cs10)
{
    static const unsigned at[3] = { OFS_NTFS_CREATED, OFS_NTFS_WRITTEN, OFS_NTFS_READ };
    (void)dir; (void)pos;
    return nt_err(ofs_ntfs_set_time(&v->u.ntfs, &n->u.ntfs, at[which], time, date, cs10));
}
#endif

static const struct ofs_fs fs_ntfs = {
    ofs_ntfs_probe, nt_mount, nt_root, nt_next, nt_lookup, nt_read, nt_statfs
#ifndef OFS_READONLY
    , nt_write, nt_resize, nt_create, nt_empty, nt_remove, nt_rename, NULL,
#ifdef OFS_NARROW
    0xFFFFFFFFUL,
#else
    ~(ofs_fsize)0,
#endif
    nt_setattr, nt_settime
#endif
    , nt_ea
#ifndef OFS_READONLY
    , NULL
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
    de->ctime = de->cdate = de->atime = de->adate = 0; de->ctime_cs = 0;
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
    , NULL, NULL, NULL, NULL, NULL, NULL, NULL, 0, NULL, NULL
#endif
    , NULL
#ifndef OFS_READONLY
    , NULL
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
    de->ctime = de->cdate = de->atime = de->adate = 0; de->ctime_cs = 0;
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
    , NULL, NULL, NULL, NULL, NULL, NULL, NULL, 0, NULL, NULL
#endif
    , NULL
#ifndef OFS_READONLY
    , NULL
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
    if (host->write && v->fs->write) {
#ifndef OFS_NO_NTFS
        if (v->fs == &fs_ntfs && !v->u.ntfs.write) return 0;   /* not safe to write */
#endif
        v->caps |= OFS_CAP_WRITE;
        if (v->fs->resize) v->caps |= OFS_CAP_RESIZE;
        if (v->fs->create) v->caps |= OFS_CAP_CREATE;
    }
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

/* ---- FAT32.IFS's extended attributes on FAT32 and exFAT ----
   OS/2's FAT32.IFS keeps a file's EAs in a file of its own beside it, its
   long name with " EA. SF" appended (hidden, system, read-only), and marks
   the file's entry (FAT32: byte 0Ch beside the NT case bits; exFAT: byte 6
   of the file entry) 40h, or 80h when one of them is critical.  The file
   holds an FEALIST: cbList (4 bytes, counting itself), then the EAs as in
   "EA DATA. SF": flags, name length, value length (word), the name, a NUL,
   the value.  Only a marked file has EAs: an EA file beside an unmarked
   one is left over (its file deleted by a system that knows nothing of
   it) and is overwritten by the first EA set.  The EA files are not shown
   and their names cannot be created; removing and renaming a file takes
   its EA file along.  Own code after the layout. */
static int sidecar(const struct ofs_vol *v)
{
    (void)v;
#ifndef OFS_NO_FAT
    if (v->fs == &fs_fat) return v->u.fat.bits == 32;
#endif
#ifndef OFS_NO_EXFAT
    if (v->fs == &fs_exfat) return 1;
#endif
    return 0;
}

static const char sf_ext[] = " EA. SF";
#define SF_EXT  7
#define SF_EAS  0x40               /* the marks */
#define SF_CRIT 0x80

static struct ofs_dirent sfe;      /* the EA file's entry */
static ofs_u16 sfn[255];           /* its name */
static unsigned sfn_len;

static int is_sf(const ofs_u16 *name, unsigned len)
{
    unsigned i;
    if (len <= SF_EXT) return 0;
    for (i = 0; i < SF_EXT; ++i) {
        ofs_u16 c = name[len - SF_EXT + i];
        if (c >= 'a' && c <= 'z') c -= 32;
        if (c != (ofs_u8)sf_ext[i]) return 0;
    }
    return 1;
}

/* the EA file's name of name into out */
static int sf_name(const ofs_u16 *name, unsigned len, ofs_u16 *out, unsigned *olen)
{
    unsigned i;
    if (len + SF_EXT > 255) return OFS_ENAME;
    memcpy(out, name, len * sizeof(ofs_u16));
    for (i = 0; i < SF_EXT; ++i) out[len + i] = (ofs_u8)sf_ext[i];
    *olen = len + SF_EXT;
    return 0;
}

static int raw_lookup(struct ofs_vol *v, struct ofs_node *dir, const ofs_u16 *name, unsigned len,
                      struct ofs_dirent *de)
{
    ofs_u32 pos = 0;
    int r;
    if (!dir->dir) return OFS_ENOTDIR;
    if (v->fs->lookup) return v->fs->lookup(v, dir, name, len, de);
    while (!(r = v->fs->next(v, dir, &pos, de)))
        if (same(de->name, de->name_len, name, len)) return 0;
    return r == OFS_END ? OFS_ENOENT : r;
}

int ofs_next(struct ofs_vol *v, struct ofs_node *dir, ofs_u32 *pos, struct ofs_dirent *de)
{
    int r, sf = sidecar(v);
    if (!dir->dir) return OFS_ENOTDIR;
    do r = v->fs->next(v, dir, pos, de);
    while (!r && sf && is_sf(de->name, de->name_len));
    return r;
}

int ofs_lookup(struct ofs_vol *v, struct ofs_node *dir, const ofs_u16 *name, unsigned len, struct ofs_dirent *de)
{
    if (sidecar(v) && is_sf(name, len)) return OFS_ENOENT;
    return raw_lookup(v, dir, name, len, de);
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

int ofs_bmap(struct ofs_vol *v, struct ofs_node *n, ofs_fsize off, ofs_lba *lba)
{
    int r = OFS_ENOTSUP;
    (void)v; (void)off; (void)lba;
    if (n->dir) return OFS_EISDIR;
#ifndef OFS_NO_FAT
    if (v->fs == &fs_fat) r = off >> 16 >> 16 ? OFS_FAT_END : ofs_fat_bmap(&v->u.fat, &n->u.fat.f, (ofs_u32)off, lba);
#endif
#ifndef OFS_NO_EXFAT
    if (v->fs == &fs_exfat) r = ofs_exfat_bmap(&v->u.ex, &n->u.ex.f, off, lba);
#endif
#ifndef OFS_NO_NTFS
    if (v->fs == &fs_ntfs) {
        r = ofs_ntfs_bmap(&v->u.ntfs, &n->u.ntfs, off, lba);
        r = r == OFS_NTFS_ENOSPACE ? OFS_ENOTSUP : nt_err(r);
    }
#endif
    return r == 1 ? OFS_ERANGE : r;
}

int ofs_statfs(struct ofs_vol *v, ofs_u32 *free_clusters, ofs_u32 *cluster_bytes, ofs_u32 *clusters)
{
    return v->fs->statfs(v, free_clusters, cluster_bytes, clusters);
}

/* the position of n's entry in its directory */
static int entry_pos(struct ofs_vol *v, const struct ofs_node *n, ofs_u32 *pos)
{
    (void)v; (void)n; (void)pos;
#ifndef OFS_NO_FAT
    if (v->fs == &fs_fat) { *pos = n->u.fat.w.pos; return 0; }
#endif
#ifndef OFS_NO_EXFAT
    if (v->fs == &fs_exfat) { *pos = n->u.ex.w.pos; return 0; }
#endif
#ifndef OFS_NO_NTFS
    if (v->fs == &fs_ntfs) { *pos = n->u.ntfs.first; return 0; }   /* the record */
#endif
    return OFS_EROFS;
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

#define CAN(v, c) if (!((v)->caps & (c))) return OFS_ENOTSUP

int ofs_resize(struct ofs_vol *v, struct ofs_node *n, ofs_fsize size)
{
    WRITABLE(v);
    CAN(v, OFS_CAP_RESIZE);
    if (n->dir) return OFS_EISDIR;
    if (size > v->fs->max_size) return OFS_EFULL;
    return v->fs->resize(v, n, size);
}

static int sf_drop(struct ofs_vol *v, struct ofs_node *dir)
{
    ofs_u32 p;
    int r = entry_pos(v, &sfe.node, &p);
    return r ? r : v->fs->remove(v, dir, p);
}

/* removes the EA file of name in dir, if there is one */
static int sf_drop_of(struct ofs_vol *v, struct ofs_node *dir, const ofs_u16 *name, unsigned len)
{
    int r;
    if ((r = sf_name(name, len, sfn, &sfn_len))) return r == OFS_ENAME ? 0 : r;
    r = raw_lookup(v, dir, sfn, sfn_len, &sfe);
    if (r) return r == OFS_ENOENT ? 0 : r;
    return sf_drop(v, dir);
}

/* 1 when d holds nothing but EA files (left over): they are removed */
static int sf_only(struct ofs_vol *v, struct ofs_node *d)
{
    ofs_u32 p = 0, at;
    int r;
    while (!(r = v->fs->next(v, d, &p, &sfe)))
        if (!is_sf(sfe.name, sfe.name_len)) return 0;
    if (r != OFS_END) return r;
    p = 0;
    while (!(r = v->fs->next(v, d, &p, &sfe)))
        if ((r = entry_pos(v, &sfe.node, &at)) || (r = v->fs->remove(v, d, at))) return r;
    return r == OFS_END ? 1 : r;
}

/* after a rename: the EA file of name in dir becomes to_name's in to */
static int sf_move(struct ofs_vol *v, struct ofs_node *dir, const ofs_u16 *name, unsigned len,
                   struct ofs_node *to, const ofs_u16 *to_name, unsigned to_len)
{
    static ofs_u16 nn[255];
    unsigned nl;
    ofs_u32 p;
    int r;
    if ((r = sf_name(to_name, to_len, nn, &nl))) return r;
    /* one left over under the new name first */
    if ((r = sf_drop_of(v, to, to_name, to_len))) return r;
    if ((r = sf_name(name, len, sfn, &sfn_len))) return r;
    r = raw_lookup(v, dir, sfn, sfn_len, &sfe);
    if (r) return r == OFS_ENOENT ? 0 : r;
    if ((r = entry_pos(v, &sfe.node, &p))) return r;
    return v->fs->rename(v, dir, p, to, nn, nl);
}

int ofs_create(struct ofs_vol *v, struct ofs_node *dir, const ofs_u16 *name, unsigned len, int is_dir,
               struct ofs_dirent *out)
{
    ofs_u32 pos;
    int r;
    WRITABLE(v);
    CAN(v, OFS_CAP_CREATE);
    if (!dir->dir) return OFS_ENOTDIR;
    if (!len || len > 255 || (sidecar(v) && is_sf(name, len))) return OFS_ENAME;
    r = ofs_lookup(v, dir, name, len, out);
    if (!r) return OFS_EEXIST;
    if (r != OFS_ENOENT) return r;
    if ((r = v->fs->create(v, dir, name, len, is_dir, &pos))) return r;
    r = v->fs->next(v, dir, &pos, out);
    return r == OFS_END ? OFS_EBAD : r;
}

/* where the entry of a node is, as the core's remove and rename want it */

int ofs_remove(struct ofs_vol *v, struct ofs_node *dir, const ofs_u16 *name, unsigned len)
{
    static struct ofs_dirent de;
    ofs_u32 pos;
    int r;
    WRITABLE(v);
    CAN(v, OFS_CAP_CREATE);
    if ((r = ofs_lookup(v, dir, name, len, &de))) return r;
    if ((r = entry_pos(v, &de.node, &pos))) return r;
    if (de.node.dir) {
        r = v->fs->empty(v, &de.node);
        if (r < 0) return r;
        if (!r && sidecar(v)) r = sf_only(v, &de.node);
        if (r < 0) return r;
        if (!r) return OFS_ENOTEMPTY;
    }
    /* the EAs first: a crash leaves the file without them, not them alone */
    if (sidecar(v) && de.eas && (r = sf_drop_of(v, dir, de.name, de.name_len))) return r;
    return v->fs->remove(v, dir, pos);
}

/* the entry's position for setattr/settime: NTFS takes the file itself */
static int meta_entry(struct ofs_vol *v, struct ofs_node *dir, const ofs_u16 *name, unsigned len,
                      struct ofs_dirent *de, ofs_u32 *pos)
{
    int r;
    if ((r = ofs_lookup(v, dir, name, len, de))) return r;
#ifndef OFS_NO_NTFS
    if (v->fs == &fs_ntfs) { *pos = 0; return 0; }
#endif
    return entry_pos(v, &de->node, pos);
}

int ofs_setattr(struct ofs_vol *v, struct ofs_node *dir, const ofs_u16 *name, unsigned len, ofs_u8 attr)
{
    static struct ofs_dirent de;
    ofs_u32 pos;
    int r;
    WRITABLE(v);
    if (!v->fs->setattr) return OFS_ENOTSUP;
    if ((r = meta_entry(v, dir, name, len, &de, &pos))) return r;
    de.node.attr = de.attr;
    return v->fs->setattr(v, dir, pos, &de.node, attr);
}

int ofs_settime(struct ofs_vol *v, struct ofs_node *dir, const ofs_u16 *name, unsigned len, unsigned which,
                ofs_u16 time, ofs_u16 date, ofs_u8 cs10)
{
    static struct ofs_dirent de;
    ofs_u32 pos;
    int r;
    WRITABLE(v);
    if (which > OFS_TIME_ACCESSED) return OFS_ENOTSUP;
    if (!v->fs->settime) return OFS_ENOTSUP;
    if ((r = meta_entry(v, dir, name, len, &de, &pos))) return r;
    return v->fs->settime(v, dir, pos, &de.node, which, time, date, cs10);
}

int ofs_rename(struct ofs_vol *v, struct ofs_node *dir, const ofs_u16 *name, unsigned len,
               struct ofs_node *to, const ofs_u16 *to_name, unsigned to_len)
{
    static struct ofs_dirent de;
    ofs_u32 pos;
    int r;
    WRITABLE(v);
    CAN(v, OFS_CAP_CREATE);
    if (!dir->dir || !to->dir) return OFS_ENOTDIR;
    if (!to_len || to_len > 255 || (sidecar(v) && is_sf(to_name, to_len))) return OFS_ENAME;
    r = ofs_lookup(v, to, to_name, to_len, &de);
    if (!r) return OFS_EEXIST;
    if (r != OFS_ENOENT) return r;
    if ((r = ofs_lookup(v, dir, name, len, &de))) return r;
    if ((r = entry_pos(v, &de.node, &pos))) return r;
    if (sidecar(v) && de.eas && to_len + SF_EXT > 255) return OFS_ENAME;   /* no name for its EAs */
    if ((r = v->fs->rename(v, dir, pos, to, to_name, to_len)) || !sidecar(v) || !de.eas) return r;
    return sf_move(v, dir, de.name, de.name_len, to, to_name, to_len);
}

int ofs_sync(struct ofs_vol *v)
{
    if (!(v->caps & OFS_CAP_WRITE) || !v->fs->sync) return 0;
    return v->fs->sync(v);
}
#endif


/* FAT32.IFS's EA file (see sidecar above): sfcb its cbList, 4 when the
   file has none */
static ofs_u8 sb[512];
static ofs_u32 sfcb;

static int sf_get(struct ofs_vol *v, ofs_u32 off, void OFS_FAR *buf, ofs_u32 len)
{
    ofs_u32 done;
    int r = ofs_read(v, &sfe.node, off, buf, len, &done);
    return r ? r : done == len ? 0 : OFS_EBAD;
}

/* the EA file of de in dir (*found: there is one); its list when de is
   marked */
static int sf_open(struct ofs_vol *v, struct ofs_node *dir, const struct ofs_dirent *de, int *found)
{
    int r;
    sfcb = 4;
    *found = 0;
    if ((r = sf_name(de->name, de->name_len, sfn, &sfn_len))) return r;
    r = raw_lookup(v, dir, sfn, sfn_len, &sfe);
    if (r) return r == OFS_ENOENT ? 0 : r;
    if (sfe.node.dir) return OFS_EBAD;
    *found = 1;
    if (!de->eas) return 0;
    if ((r = sf_get(v, 0, sb, 4))) return r;
    sfcb = sb[0] | (ofs_u32)sb[1] << 8 | (ofs_u32)sb[2] << 16 | (ofs_u32)sb[3] << 24;
    if (sfcb < 4 || sfcb > 0xFFFFUL || sfcb > sfe.node.size) return OFS_EBAD;
    return 0;
}

/* the EA at *off: flags, name (sb + 4, NUL-terminated), value length; *off
   moves past it */
static int sf_next(struct ofs_vol *v, ofs_u32 *off, unsigned *nl, unsigned *vl)
{
    int r;
    if (*off + 4 > sfcb) return OFS_EBAD;
    if ((r = sf_get(v, *off, sb, 4))) return r;
    *nl = sb[1];
    *vl = sb[2] | sb[3] << 8;
    if (*off + 5 + *nl + *vl > sfcb) return OFS_EBAD;
    if ((r = sf_get(v, *off + 4, sb + 4, *nl + 1UL))) return r;
    if (sb[4 + *nl]) return OFS_EBAD;
    *off += 5UL + *nl + *vl;
    return 0;
}

static int sf_same(const char *a, unsigned n)
{
    unsigned i;
    for (i = 0; i < n; ++i) {
        unsigned c = (ofs_u8)a[i], d = sb[4 + i];
        if (c >= 'a' && c <= 'z') c -= 32;
        if (d >= 'a' && d <= 'z') d -= 32;
        if (c != d) return 0;
    }
    return !a[n];
}

/* the EA name: its offset and size (0: none) */
static int sf_find(struct ofs_vol *v, const char *name, ofs_u32 *at, ofs_u32 *size)
{
    ofs_u32 off = 4, a;
    unsigned nl, vl;
    int r;
    *size = 0;
    while (off < sfcb) {
        a = off;
        if ((r = sf_next(v, &off, &nl, &vl))) return r;
        if (sf_same(name, nl)) { *at = a; *size = off - a; return 0; }
    }
    return 0;
}

#ifndef OFS_READONLY
/* copies len bytes inside the EA file from src down to dst */
static int sf_copy(struct ofs_vol *v, ofs_u32 dst, ofs_u32 src, ofs_u32 len)
{
    ofs_u32 n;
    int r;
    while (len) {
        n = len > sizeof(sb) ? sizeof(sb) : len;
        if ((r = sf_get(v, src, sb, n)) || (r = ofs_write(v, &sfe.node, dst, sb, n))) return r;
        src += n; dst += n; len -= n;
    }
    return 0;
}

/* Changes the list in place (as FAT32.IFS does; a crash in between can
   leave a damaged list, OFS_EBAD until the file's EAs are removed); the
   mark is set last when the file is new and cleared first when the last
   EA goes, so the EA file never belongs to a file it does not know of. */
static int sf_set(struct ofs_vol *v, struct ofs_node *dir, const struct ofs_dirent *de, ofs_u32 pos,
                  const char *name, const void OFS_FAR *val, ofs_u16 vlen, int how)
{
    ofs_u32 at = 0, sz = 0, ncb, end, off;
    unsigned nl = (unsigned)strlen(name), enl, evl;
    ofs_u8 mark = SF_EAS;
    int found, r;
    if (!nl || nl > 255) return OFS_ENAME;
    if ((r = sf_open(v, dir, de, &found))) return r;
    if (de->eas && found && (r = sf_find(v, name, &at, &sz))) return r;
    if (how == OFS_EA_CREATE && sz) return OFS_EEXIST;
    if ((how == OFS_EA_REPLACE || how == OFS_EA_REMOVE) && !sz) return OFS_ENOATTR;
    ncb = sfcb - sz + (how == OFS_EA_REMOVE ? 0 : 5UL + nl + vlen);
    if (ncb > 0xFFFFUL) return OFS_EFULL;
    if (ncb == 4) {
        if (de->eas && (r = v->fs->eamark(v, dir, pos, 0))) return r;
        return found ? sf_drop(v, dir) : 0;
    }
    if (!found) {
        ofs_u32 p;
        if ((r = v->fs->create(v, dir, sfn, sfn_len, 0, &p))) return r;
        if ((r = v->fs->next(v, dir, &p, &sfe))) return r == OFS_END ? OFS_EBAD : r;
    }
    end = sfcb;
    if (sz) {
        if ((r = sf_copy(v, at, at + sz, end - at - sz))) return r;
        end -= sz;
    }
    if (how != OFS_EA_REMOVE) {
        sb[0] = 0;
        sb[1] = (ofs_u8)nl;
        sb[2] = (ofs_u8)vlen;
        sb[3] = (ofs_u8)(vlen >> 8);
        memcpy(sb + 4, name, nl + 1);
        if ((r = ofs_write(v, &sfe.node, end, sb, 5UL + nl))) return r;
        if (vlen && (r = ofs_write(v, &sfe.node, end + 5 + nl, val, vlen))) return r;
        end += 5UL + nl + vlen;
    }
    sb[0] = (ofs_u8)ncb; sb[1] = (ofs_u8)(ncb >> 8); sb[2] = sb[3] = 0;
    if ((r = ofs_write(v, &sfe.node, 0, sb, 4))) return r;
    if (sfe.node.size > ncb && (r = ofs_resize(v, &sfe.node, ncb))) return r;
    sfcb = ncb;
    if (!found) {
        ofs_u32 p;
        if ((r = entry_pos(v, &sfe.node, &p))) return r;
        sfe.node.attr = sfe.attr;
        if ((r = v->fs->setattr(v, dir, p, &sfe.node, OFS_ATTR_HIDDEN | OFS_ATTR_SYSTEM | OFS_ATTR_RDONLY)))
            return r;
    }
    /* critical: one of them flagged "need" */
    for (off = 4; off < sfcb;) {
        if ((r = sf_next(v, &off, &enl, &evl))) return r;
        if (sb[0] & 0x80) mark = SF_CRIT;
    }
    return de->eas == mark ? 0 : v->fs->eamark(v, dir, pos, mark);
}
#endif

static int sf_ea(struct ofs_vol *v, struct ofs_node *dir, const struct ofs_dirent *de, ofs_u32 pos, int op,
                 const char *name, void OFS_FAR *buf, ofs_u16 size, ofs_u16 *len)
{
    ofs_u32 at, sz, off, total = 0;
    unsigned nl, vl;
    int found, r;
    if (op == EA_GET || op == EA_LIST) {
        if (!de->eas) return op == EA_GET ? OFS_ENOATTR : 0;
        if ((r = sf_open(v, dir, de, &found))) return r;
        if (!found) return op == EA_GET ? OFS_ENOATTR : 0;
    }
    if (op == EA_GET) {
        if ((r = sf_find(v, name, &at, &sz))) return r;
        if (!sz) return OFS_ENOATTR;
        if ((r = sf_get(v, at, sb, 4))) return r;
        *len = (ofs_u16)(sb[2] | sb[3] << 8);
        if (!size) return 0;
        if (*len > size) return OFS_ERANGE;
        return sf_get(v, at + 5 + sb[1], buf, *len);
    }
    if (op == EA_LIST) {
        for (off = 4; off < sfcb;) {
            if ((r = sf_next(v, &off, &nl, &vl))) return r;
            if (size) {
                unsigned i;
                if (total + nl + 1 > size) return OFS_ERANGE;
                for (i = 0; i <= nl; ++i) ((char OFS_FAR *)buf)[total + i] = (char)sb[4 + i];
            }
            total += nl + 1;
            if (total > 0xFFFFUL) return OFS_ERANGE;
        }
        *len = (ofs_u16)total;
        return 0;
    }
#ifdef OFS_READONLY
    (void)pos;
    return OFS_EROFS;
#else
    return sf_set(v, dir, de, pos, name, buf, size, op);
#endif
}

/* the entry of name in dir for its extended attributes */
static int ea_call(struct ofs_vol *v, struct ofs_node *dir, const ofs_u16 *name, unsigned len, int op,
                   const char *ea, void OFS_FAR *buf, ofs_u16 size, ofs_u16 *out)
{
    static struct ofs_dirent de;
    ofs_u32 pos;
    int r;
    *out = 0;
    if (!v->fs->ea && !sidecar(v)) return OFS_ENOTSUP;
#ifndef OFS_NO_NTFS
    if (!len && v->fs == &fs_ntfs) return v->fs->ea(v, dir, 0, dir, op, ea, buf, size, out);   /* dir itself */
#endif
    if ((r = ofs_lookup(v, dir, name, len, &de))) return r;
#ifndef OFS_NO_NTFS
    if (v->fs == &fs_ntfs) pos = 0;               /* NTFS takes the file itself */
    else
#endif
    if ((r = entry_pos(v, &de.node, &pos))) return r;
    if (sidecar(v)) return sf_ea(v, dir, &de, pos, op, ea, buf, size, out);
    return v->fs->ea(v, dir, pos, &de.node, op, ea, buf, size, out);
}

int ofs_getea(struct ofs_vol *v, struct ofs_node *dir, const ofs_u16 *name, unsigned len, const char *ea,
              void OFS_FAR *buf, ofs_u16 size, ofs_u16 *vlen)
{
    return ea_call(v, dir, name, len, EA_GET, ea, buf, size, vlen);
}

int ofs_listea(struct ofs_vol *v, struct ofs_node *dir, const ofs_u16 *name, unsigned len,
               char OFS_FAR *buf, ofs_u16 size, ofs_u16 *total)
{
    return ea_call(v, dir, name, len, EA_LIST, NULL, buf, size, total);
}

#ifndef OFS_READONLY
int ofs_setea(struct ofs_vol *v, struct ofs_node *dir, const ofs_u16 *name, unsigned len, const char *ea,
              const void OFS_FAR *val, ofs_u16 vlen, int how)
{
    ofs_u16 n;
    WRITABLE(v);
    if (how < OFS_EA_SET || how > OFS_EA_REMOVE) return OFS_ENOTSUP;
    return ea_call(v, dir, name, len, how, ea, (void OFS_FAR *)val, vlen, &n);
}
#endif
