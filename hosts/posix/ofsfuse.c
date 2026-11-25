/* SPDX-License-Identifier: GPL-2.0-or-later */
/* ofsfuse: mounts a FAT, exFAT or NTFS volume of an image (or block
   device) through FUSE 3, with OpenFileSystem's cores behind the common
   interface (include/ofs/vol.h) -- the same one the DOS redirector and
   GonWare use. It is for testing the cores on Linux: what cp, diff, rsync
   or find do through it, the cores do.

     ofsfuse [-o part=N] [-o ro] [-o sector=512] [-o cp=850] [-o unitab=DIR] IMAGE MOUNTPOINT [FUSE options]

   part=N: the N-th partition of an MBR/GPT (in table order, logical ones
   after the primary ones); without it a volume over the whole image (a
   superfloppy, an ISO 9660 image -- also a hybrid one with a partition
   table), else the first partition with a file system the cores know.
   sector: the sector size the image is read with, 512 to 4096 (2048: as
   a CD drive gives it).  cp: the code page of the
   8.3 names, from UNI_<cp>.TAB in unitab (mars-unicode-tables); without the
   table Latin-1.  NTFS is read-only.  It runs single-threaded (the cores
   take calls one at a time).  Times of new entries are the local time;
   setting times and modes is accepted and ignored. */
#define FUSE_USE_VERSION 31
#define _FILE_OFFSET_BITS 64
#include <errno.h>
#include <fcntl.h>
#include <fuse.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/xattr.h>
#include <time.h>
#include <unistd.h>
#ifndef RENAME_NOREPLACE
#define RENAME_NOREPLACE (1 << 0)   /* linux/fs.h */
#endif
#include "ofs/part.h"
#include "ofs/vol.h"

#ifndef UNITAB
#define UNITAB "/usr/share/mars-unicode-tables"
#endif

static struct {
    char *image, *unitab, *cpname;
    int part, ro, sector;
} opt;

static int fd = -1;
static unsigned ss = 512;
static struct ofs_vol vol;
static struct ofs_cp cp;
static ofs_u8 vbuf[OFS_VOL_BUF];

/* ---- the host: sectors of the image ---- */

static int rd(void *dev, ofs_lba lba, void *buf)
{
    (void)dev;
    return pread(fd, buf, ss, (off_t)lba * ss) == (ssize_t)ss ? 0 : -1;
}

static int wr(void *dev, ofs_lba lba, const void *buf)
{
    (void)dev;
    return pwrite(fd, buf, ss, (off_t)lba * ss) == (ssize_t)ss ? 0 : -1;
}

static int prd(void *dev, ofs_u64 lba, void *buf) { return rd(dev, lba, buf); }

static void now(void *dev, ofs_u16 *t, ofs_u16 *d)
{
    time_t s = time(NULL);
    struct tm tm;
    (void)dev;
    localtime_r(&s, &tm);
    *t = (ofs_u16)(tm.tm_hour << 11 | tm.tm_min << 5 | tm.tm_sec / 2);
    *d = (ofs_u16)((tm.tm_year - 80) << 9 | (tm.tm_mon + 1) << 5 | tm.tm_mday);
}

/* ---- names ---- */

/* UTF-8 to UTF-16 (surrogates above the BMP); -1 when too long */
static int to16(const char *s, size_t n, ofs_u16 *out)
{
    const unsigned char *p = (const unsigned char *)s, *e = p + n;
    int k = 0;
    while (p < e) {
        unsigned long c;
        if (*p < 0x80) c = *p++;
        else if ((*p & 0xE0) == 0xC0 && p + 1 < e) { c = (unsigned long)(p[0] & 31) << 6 | (p[1] & 63); p += 2; }
        else if ((*p & 0xF0) == 0xE0 && p + 2 < e) {
            c = (unsigned long)(p[0] & 15) << 12 | (unsigned long)(p[1] & 63) << 6 | (p[2] & 63); p += 3;
        } else if ((*p & 0xF8) == 0xF0 && p + 3 < e) {
            c = (unsigned long)(p[0] & 7) << 18 | (unsigned long)(p[1] & 63) << 12 | (unsigned long)(p[2] & 63) << 6 |
                (p[3] & 63);
            p += 4;
        } else return -1;
        if (c >= 0x10000) {
            if (k + 2 > 255) return -1;
            c -= 0x10000;
            out[k++] = (ofs_u16)(0xD800 | c >> 10);
            out[k++] = (ofs_u16)(0xDC00 | (c & 0x3FF));
        } else {
            if (k + 1 > 255) return -1;
            out[k++] = (ofs_u16)c;
        }
    }
    return k;
}

static void to8(const ofs_u16 *s, unsigned n, char *out)
{
    unsigned i;
    char *o = out;
    for (i = 0; i < n; ++i) {
        unsigned long c = s[i];
        if (c >= 0xD800 && c < 0xDC00 && i + 1 < n && s[i + 1] >= 0xDC00 && s[i + 1] < 0xE000) {
            c = 0x10000 + ((c - 0xD800) << 10 | (s[i + 1] - 0xDC00));
            ++i;
        }
        if (c < 0x80) *o++ = (char)c;
        else if (c < 0x800) { *o++ = (char)(0xC0 | c >> 6); *o++ = (char)(0x80 | (c & 63)); }
        else if (c < 0x10000) {
            *o++ = (char)(0xE0 | c >> 12); *o++ = (char)(0x80 | (c >> 6 & 63)); *o++ = (char)(0x80 | (c & 63));
        } else {
            *o++ = (char)(0xF0 | c >> 18); *o++ = (char)(0x80 | (c >> 12 & 63));
            *o++ = (char)(0x80 | (c >> 6 & 63)); *o++ = (char)(0x80 | (c & 63));
        }
    }
    *o = 0;
}

static int err(int r)
{
    switch (r) {
    case OFS_OK: return 0;
    case OFS_ENOENT: return -ENOENT;
    case OFS_ENOTDIR: return -ENOTDIR;
    case OFS_EISDIR: return -EISDIR;
    case OFS_EEXIST: return -EEXIST;
    case OFS_ENAME: return -EINVAL;
    case OFS_EFULL: return -ENOSPC;
    case OFS_EROFS: return -EROFS;
    case OFS_ENOTSUP: return -EOPNOTSUPP;
    case OFS_ENOTEMPTY: return -ENOTEMPTY;
    case OFS_ENOATTR: return -ENODATA;
    case OFS_ERANGE: return -ERANGE;
    default: return -EIO;
    }
}

/* The node of a path; with parent set, the directory of the last part,
   that part in name/len instead (it need not exist). */
static int walk(const char *path, struct ofs_node *out, ofs_u16 *name, int *len)
{
    static struct ofs_dirent de;
    ofs_u16 n16[255];
    const char *p = path, *q;
    int r, k;
    ofs_root(&vol, out);
    for (;;) {
        while (*p == '/') ++p;
        if (!*p) return name ? -EINVAL : 0;
        for (q = p; *q && *q != '/'; ++q)
            ;
        k = to16(p, (size_t)(q - p), n16);
        if (k < 0) return -ENAMETOOLONG;
        {
            const char *rest = q;
            while (*rest == '/') ++rest;
            if (!*rest && name) {
                memcpy(name, n16, (size_t)k * sizeof(ofs_u16));
                *len = k;
                return out->dir ? 0 : -ENOTDIR;
            }
        }
        r = ofs_lookup(&vol, out, n16, (unsigned)k, &de);
        if (r) return err(r);
        *out = de.node;
        p = q;
    }
}

static time_t dostime(ofs_u16 t, ofs_u16 d)
{
    struct tm tm;
    if (!d) return 0;
    memset(&tm, 0, sizeof(tm));
    tm.tm_year = (d >> 9) + 80;
    tm.tm_mon = (d >> 5 & 15) - 1;
    tm.tm_mday = d & 31;
    tm.tm_hour = t >> 11;
    tm.tm_min = t >> 5 & 63;
    tm.tm_sec = (t & 31) * 2;
    tm.tm_isdst = -1;
    return mktime(&tm);
}

static void fill(struct stat *st, const struct ofs_node *n, ofs_u16 t, ofs_u16 d)
{
    memset(st, 0, sizeof(*st));
    st->st_mode = n->dir ? S_IFDIR | 0755 : S_IFREG | ((n->attr & OFS_ATTR_RDONLY) ? 0444 : 0644);
    if (!(vol.caps & OFS_CAP_WRITE)) st->st_mode &= ~0222u;
    st->st_nlink = n->dir ? 2 : 1;
    st->st_size = (off_t)n->size;
    st->st_blocks = (blkcnt_t)((n->size + 511) / 512);
    st->st_uid = getuid();
    st->st_gid = getgid();
    st->st_mtime = st->st_ctime = st->st_atime = dostime(t, d);
}

/* the entry of a path, with its times (the root has none) */
static int entry(const char *path, struct ofs_dirent *de)
{
    ofs_u16 name[255];
    int len, r;
    struct ofs_node dir;
    if (!strcmp(path, "/")) {
        memset(de, 0, sizeof(*de));
        return ofs_root(&vol, &de->node);
    }
    if ((r = walk(path, &dir, name, &len))) return r;
    return err(ofs_lookup(&vol, &dir, name, (unsigned)len, de));
}

/* ---- FUSE ---- */

static void *op_init(struct fuse_conn_info *conn, struct fuse_config *cfg)
{
    (void)conn;
    cfg->use_ino = 0;
    cfg->kernel_cache = 0;
    /* no cached sizes and entries: a change can change another file too
       (an extended attribute, "EA DATA. SF") */
    cfg->attr_timeout = 0;
    cfg->entry_timeout = 0;
    cfg->negative_timeout = 0;
    return NULL;
}

static void op_destroy(void *p)
{
    (void)p;
#ifndef OFS_READONLY
    ofs_sync(&vol);
#endif
    fsync(fd);
}

static int op_getattr(const char *path, struct stat *st, struct fuse_file_info *fi)
{
    static struct ofs_dirent de;
    int r;
    (void)fi;
    if ((r = entry(path, &de))) return r;
    fill(st, &de.node, de.time, de.date);
    return 0;
}

static int op_readdir(const char *path, void *buf, fuse_fill_dir_t filler, off_t off, struct fuse_file_info *fi,
                      enum fuse_readdir_flags flags)
{
    static struct ofs_dirent de;
    static char name[255 * 4 + 1];
    struct ofs_node dir;
    struct stat st;
    ofs_u32 pos = 0;
    int r;
    (void)off; (void)fi; (void)flags;
    if ((r = walk(path, &dir, NULL, NULL))) return r;
    if (!dir.dir) return -ENOTDIR;
    filler(buf, ".", NULL, 0, 0);
    filler(buf, "..", NULL, 0, 0);
    while (!(r = ofs_next(&vol, &dir, &pos, &de))) {
        to8(de.name, de.name_len, name);
        fill(&st, &de.node, de.time, de.date);
        if (filler(buf, name, &st, 0, 0)) break;
    }
    return r == OFS_END || r == 0 ? 0 : err(r);
}

static int op_open(const char *path, struct fuse_file_info *fi)
{
    static struct ofs_dirent de;
    struct ofs_node *n;
    int r;
    if ((r = entry(path, &de))) return r;
    if (de.node.dir) return -EISDIR;
    if ((fi->flags & O_ACCMODE) != O_RDONLY && !(vol.caps & OFS_CAP_WRITE)) return -EROFS;
    if (!(n = malloc(sizeof(*n)))) return -ENOMEM;
    *n = de.node;
#ifndef OFS_READONLY
    if (fi->flags & O_TRUNC) {
        r = ofs_resize(&vol, n, 0);
        if (r) { free(n); return err(r); }
    }
#endif
    fi->fh = (uint64_t)(uintptr_t)n;
    return 0;
}

static int op_release(const char *path, struct fuse_file_info *fi)
{
    (void)path;
    free((void *)(uintptr_t)fi->fh);
    return 0;
}

static int op_read(const char *path, char *buf, size_t size, off_t off, struct fuse_file_info *fi)
{
    struct ofs_node *n = (struct ofs_node *)(uintptr_t)fi->fh;
    ofs_u32 done;
    int r;
    (void)path;
    if ((ofs_fsize)off >= n->size) return 0;
    r = ofs_read(&vol, n, (ofs_fsize)off, buf, (ofs_u32)size, &done);
    return r ? err(r) : (int)done;
}

static int op_statfs(const char *path, struct statvfs *sv)
{
    ofs_u32 fr = 0, cb = 512, cl = 0;
    (void)path;
    memset(sv, 0, sizeof(*sv));
    if (ofs_statfs(&vol, &fr, &cb, &cl)) fr = 0;
    sv->f_bsize = sv->f_frsize = cb;
    sv->f_blocks = cl;
    sv->f_bfree = sv->f_bavail = fr == 0xFFFFFFFFUL ? 0 : fr;
    sv->f_namemax = 255;
    return 0;
}

#ifndef OFS_READONLY
static int op_write(const char *path, const char *buf, size_t size, off_t off, struct fuse_file_info *fi)
{
    struct ofs_node *n = (struct ofs_node *)(uintptr_t)fi->fh;
    int r;
    (void)path;
    if (fi->flags & O_APPEND) off = (off_t)n->size;
    r = ofs_write(&vol, n, (ofs_fsize)off, buf, (ofs_u32)size);
    return r ? err(r) : (int)size;
}

static int make(const char *path, int is_dir, struct ofs_dirent *de)
{
    ofs_u16 name[255];
    struct ofs_node dir;
    int len, r;
    if ((r = walk(path, &dir, name, &len))) return r;
    return err(ofs_create(&vol, &dir, name, (unsigned)len, is_dir, de));
}

static int op_create(const char *path, mode_t mode, struct fuse_file_info *fi)
{
    static struct ofs_dirent de;
    struct ofs_node *n;
    int r;
    (void)mode;
    if ((r = make(path, 0, &de))) return r;
    if (!(n = malloc(sizeof(*n)))) return -ENOMEM;
    *n = de.node;
    fi->fh = (uint64_t)(uintptr_t)n;
    return 0;
}

static int op_mkdir(const char *path, mode_t mode)
{
    static struct ofs_dirent de;
    (void)mode;
    return make(path, 1, &de);
}

static int drop(const char *path, int want_dir)
{
    static struct ofs_dirent de;
    ofs_u16 name[255];
    struct ofs_node dir;
    int len, r;
    if ((r = walk(path, &dir, name, &len))) return r;
    if ((r = ofs_lookup(&vol, &dir, name, (unsigned)len, &de))) return err(r);
    if (want_dir && !de.node.dir) return -ENOTDIR;
    if (!want_dir && de.node.dir) return -EISDIR;
    return err(ofs_remove(&vol, &dir, name, (unsigned)len));
}

static int op_unlink(const char *path) { return drop(path, 0); }
static int op_rmdir(const char *path) { return drop(path, 1); }

static int op_rename(const char *from, const char *to, unsigned int flags)
{
    static struct ofs_dirent de, src;
    ofs_u16 n1[255], n2[255];
    struct ofs_node d1, d2;
    int l1, l2, r;
    if (flags & ~(unsigned)RENAME_NOREPLACE) return -EINVAL;
    if ((r = walk(from, &d1, n1, &l1)) || (r = walk(to, &d2, n2, &l2))) return r;
    if ((r = ofs_lookup(&vol, &d1, n1, (unsigned)l1, &src))) return err(r);
    r = ofs_lookup(&vol, &d2, n2, (unsigned)l2, &de);
    if (!r) {
        /* POSIX: the target is replaced (a directory only by an empty one) */
        if (flags & RENAME_NOREPLACE) return -EEXIST;
        if (de.node.dir != src.node.dir) return de.node.dir ? -EISDIR : -ENOTDIR;
        if ((r = ofs_remove(&vol, &d2, n2, (unsigned)l2))) return err(r);
        /* the removal may have moved entries of d1 when it is d2: walk again */
        if ((r = walk(from, &d1, n1, &l1)) || (r = walk(to, &d2, n2, &l2))) return r;
    } else if (r != OFS_ENOENT) return err(r);
    return err(ofs_rename(&vol, &d1, n1, (unsigned)l1, &d2, n2, (unsigned)l2));
}

static int op_truncate(const char *path, off_t size, struct fuse_file_info *fi)
{
    static struct ofs_dirent de;
    int r;
    if (fi && fi->fh) return err(ofs_resize(&vol, (struct ofs_node *)(uintptr_t)fi->fh, (ofs_fsize)size));
    if ((r = entry(path, &de))) return r;
    return err(ofs_resize(&vol, &de.node, (ofs_fsize)size));
}

static int op_fsync(const char *path, int datasync, struct fuse_file_info *fi)
{
    (void)path; (void)datasync; (void)fi;
    return err(ofs_sync(&vol)) ? -EIO : (fsync(fd) ? -errno : 0);
}
#endif

#ifndef OFS_READONLY
/* a time as the local DOS form and its 10 ms units (0/0 before 1980) */
static void todos(const struct timespec *ts, ofs_u16 *t, ofs_u16 *d, ofs_u8 *cs10)
{
    struct tm tm;
    time_t s = ts->tv_sec;
    *t = *d = 0; *cs10 = 0;
    if (!localtime_r(&s, &tm) || tm.tm_year < 80 || tm.tm_year > 207) return;
    *d = (ofs_u16)((tm.tm_year - 80) << 9 | (tm.tm_mon + 1) << 5 | tm.tm_mday);
    *t = (ofs_u16)(tm.tm_hour << 11 | tm.tm_min << 5 | tm.tm_sec / 2);
    *cs10 = (ofs_u8)((tm.tm_sec & 1) * 100 + ts->tv_nsec / 10000000);
}

/* tv[0] the access, tv[1] the modification time (UTIME_NOW, UTIME_OMIT) */
static int op_utimens(const char *path, const struct timespec tv[2], struct fuse_file_info *fi)
{
    static const unsigned which[2] = { OFS_TIME_ACCESSED, OFS_TIME_MODIFIED };
    ofs_u16 name[255], t, d;
    ofs_u8 cs;
    struct ofs_node dir;
    struct timespec now_ts, ts;
    int len, r, i;
    (void)fi;
    if (!strcmp(path, "/")) return 0;            /* the root has no times */
    if ((r = walk(path, &dir, name, &len))) return r;
    clock_gettime(CLOCK_REALTIME, &now_ts);
    for (i = 0; i < 2; ++i) {
        if (tv && tv[i].tv_nsec == UTIME_OMIT) continue;
        ts = !tv || tv[i].tv_nsec == UTIME_NOW ? now_ts : tv[i];
        todos(&ts, &t, &d, &cs);
        if (!d) return -EINVAL;
        if ((r = ofs_settime(&vol, &dir, name, (unsigned)len, which[i], t, d, cs))) return err(r);
    }
    return 0;
}

/* the owner's write bit is the read-only attribute; the rest has no place
   (fill shows 0644/0444) */
static int op_chmod(const char *path, mode_t mode, struct fuse_file_info *fi)
{
    static struct ofs_dirent de;
    ofs_u16 name[255];
    struct ofs_node dir;
    int len, r;
    (void)fi;
    if (!strcmp(path, "/")) return 0;
    if ((r = walk(path, &dir, name, &len))) return r;
    if ((r = ofs_lookup(&vol, &dir, name, (unsigned)len, &de))) return err(r);
    return err(ofs_setattr(&vol, &dir, name, (unsigned)len,
                           (ofs_u8)((mode & 0200) ? de.attr & ~OFS_ATTR_RDONLY : de.attr | OFS_ATTR_RDONLY)));
}
#else
static int op_utimens(const char *path, const struct timespec tv[2], struct fuse_file_info *fi)
{
    (void)path; (void)tv; (void)fi;
    return -EROFS;
}

static int op_chmod(const char *path, mode_t mode, struct fuse_file_info *fi)
{
    (void)path; (void)mode; (void)fi;
    return -EROFS;
}
#endif

static int op_chown(const char *path, uid_t uid, gid_t gid, struct fuse_file_info *fi)
{
    (void)path; (void)uid; (void)gid; (void)fi;
    return 0;
}

/* Extended attributes: "user.NAME" is the EA NAME (OS/2's and NT's on
   FAT12/16); other name spaces have none. */
static const char *ea_name(const char *name)
{
    return strncmp(name, "user.", 5) || !name[5] ? NULL : name + 5;
}

static int op_getxattr(const char *path, const char *name, char *value, size_t size)
{
    ofs_u16 n[255], len;
    struct ofs_node dir;
    int l, r;
    const char *ea = ea_name(name);
    if (!ea || !strcmp(path, "/")) return -ENODATA;
    if ((r = walk(path, &dir, n, &l))) return r;
    r = ofs_getea(&vol, &dir, n, (unsigned)l, ea, value, (ofs_u16)(size > 0xFFFF ? 0xFFFF : size), &len);
    return r == OFS_ENOTSUP ? -ENODATA : r ? err(r) : (int)len;
}

static int op_listxattr(const char *path, char *list, size_t size)
{
    static char names[65536];
    ofs_u16 n[255], len;
    struct ofs_node dir;
    size_t total = 0, i = 0, k;
    int l, r;
    if (!strcmp(path, "/")) return 0;
    if ((r = walk(path, &dir, n, &l))) return r;
    r = ofs_listea(&vol, &dir, n, (unsigned)l, names, 0xFFFF, &len);
    if (r == OFS_ENOTSUP) return 0;
    if (r) return err(r);
    while (i < len) {
        k = strlen(names + i) + 1;
        if (size) {
            if (total + 5 + k > size) return -ERANGE;
            memcpy(list + total, "user.", 5);
            memcpy(list + total + 5, names + i, k);
        }
        total += 5 + k;
        i += k;
    }
    return (int)total;
}

#ifndef OFS_READONLY
static int op_setxattr(const char *path, const char *name, const char *value, size_t size, int flags)
{
    ofs_u16 n[255];
    struct ofs_node dir;
    int l, r, how = flags & XATTR_CREATE ? OFS_EA_CREATE : flags & XATTR_REPLACE ? OFS_EA_REPLACE : OFS_EA_SET;
    const char *ea = ea_name(name);
    if (!ea || !strcmp(path, "/")) return -EOPNOTSUPP;
    if (size > 0xFFFF) return -E2BIG;
    if ((r = walk(path, &dir, n, &l))) return r;
    return err(ofs_setea(&vol, &dir, n, (unsigned)l, ea, value, (ofs_u16)size, how));
}

static int op_removexattr(const char *path, const char *name)
{
    ofs_u16 n[255];
    struct ofs_node dir;
    int l, r;
    const char *ea = ea_name(name);
    if (!ea || !strcmp(path, "/")) return -ENODATA;
    if ((r = walk(path, &dir, n, &l))) return r;
    return err(ofs_setea(&vol, &dir, n, (unsigned)l, ea, NULL, 0, OFS_EA_REMOVE));
}
#endif

static struct fuse_operations ops;

/* ---- start ---- */

struct pick { int want, n; ofs_u64 start; int found; };

static int pick_part(void *ctx, const struct ofs_part *p)
{
    struct pick *k = ctx;
    ofs_u8 b[4096];
    ++k->n;
    if (k->want) {
        if (k->n != k->want) return 0;
        k->start = p->start; k->found = 1;
        return 1;
    }
    if (rd(NULL, p->start, b) == 0 && ofs_probe(b)) { k->start = p->start; k->found = 1; return 1; }
    return 0;
}

static void load_cp(void)
{
    char path[4096];
    FILE *f;
    static ofs_u8 tab[8192];
    size_t n;
    unsigned i, number = (unsigned)atoi(opt.cpname ? opt.cpname : "850");
    snprintf(path, sizeof(path), "%s/UNI_%u.TAB", opt.unitab ? opt.unitab : UNITAB, number);
    if ((f = fopen(path, "rb"))) {
        n = fread(tab, 1, sizeof(tab), f);
        fclose(f);
        if (!ofs_cp_load(&cp, (ofs_u16)number, tab, (ofs_u32)n)) return;
    }
    fprintf(stderr, "ofsfuse: no code page table %s, Latin-1 for the 8.3 names\n", path);
    cp.cp = 1252;
    for (i = 0; i < 128; ++i) { cp.uni[i] = (ofs_u16)(0x80 + i); cp.upper[i] = (ofs_u8)(0x80 + i); }
}

static const struct fuse_opt specs[] = {
    { "part=%d", offsetof(__typeof__(opt), part), 0 },
    { "sector=%d", offsetof(__typeof__(opt), sector), 0 },
    { "unitab=%s", offsetof(__typeof__(opt), unitab), 0 },
    { "cp=%s", offsetof(__typeof__(opt), cpname), 0 },
    { "ro", offsetof(__typeof__(opt), ro), 1 },
    FUSE_OPT_END
};

static int nonopt(void *data, const char *arg, int key, struct fuse_args *out)
{
    (void)data; (void)out;
    if (key == FUSE_OPT_KEY_NONOPT && !opt.image) {
        opt.image = strdup(arg);
        return 0;
    }
    if (key == FUSE_OPT_KEY_OPT && !strcmp(arg, "ro")) opt.ro = 1;
    return 1;
}

int main(int argc, char **argv)
{
    struct fuse_args args = FUSE_ARGS_INIT(argc, argv);
    struct ofs_host host;
    struct ofs_part_disk disk;
    struct pick k;
    ofs_u8 b[4096];
    ofs_lba start = 0;
    int r;

    if (fuse_opt_parse(&args, &opt, specs, nonopt) || !opt.image) {
        fprintf(stderr, "usage: ofsfuse [-o part=N] [-o ro] [-o sector=512] [-o cp=850] [-o unitab=DIR] IMAGE MOUNTPOINT\n");
        return 2;
    }
    if ((fd = open(opt.image, opt.ro ? O_RDONLY : O_RDWR)) < 0) {
        perror(opt.image);
        return 1;
    }
    load_cp();
    memset(&k, 0, sizeof(k));
    k.want = opt.part;
    if (opt.sector) {
        if (opt.sector < 512 || opt.sector > 4096 || (opt.sector & (opt.sector - 1))) {
            fprintf(stderr, "ofsfuse: sector=%d: 512, 1024, 2048 or 4096\n", opt.sector);
            return 2;
        }
        ss = (unsigned)opt.sector;
    }
    if (rd(NULL, 0, b)) { fprintf(stderr, "ofsfuse: %s: unreadable\n", opt.image); return 1; }
    memset(&host, 0, sizeof(host));
    while ((1u << host.sshift) < ss) ++host.sshift;
    host.read = rd;
    host.write = opt.ro ? NULL : wr;
    host.now = now;
    {
        time_t s = time(NULL);
        struct tm tm;
        localtime_r(&s, &tm);
        host.tz = (ofs_s16)(tm.tm_gmtoff / 60);
    }
    /* the whole image first (a superfloppy, ISO 9660), then a partition */
    r = opt.part ? OFS_EBAD : ofs_mount(&vol, &host, NULL, 0, &cp, vbuf);
    if (r == OFS_EBAD) {
        r = ofs_part_scan(prd, NULL, host.sshift, (ofs_u64)(lseek(fd, 0, SEEK_END) / ss), b, &disk, pick_part, &k);
        if (r < 0 || !k.found) {
            fprintf(stderr, "ofsfuse: %s: %s\n", opt.image, opt.part ? "no such partition" : "no volume found");
            return 1;
        }
        start = k.start;
        r = ofs_mount(&vol, &host, NULL, start, &cp, vbuf);
    }
    if (r) {
        fprintf(stderr, "ofsfuse: %s: not mounted (%d)\n", opt.image, r);
        return 1;
    }
    fprintf(stderr, "ofsfuse: %s \"%s\"%s%s\n", vol.type, vol.label, vol.caps & OFS_CAP_WRITE ? "" : ", read-only",
            vol.dirty ? ", not cleanly unmounted" : "");
    ops.init = op_init;
    ops.destroy = op_destroy;
    ops.getattr = op_getattr;
    ops.readdir = op_readdir;
    ops.open = op_open;
    ops.release = op_release;
    ops.read = op_read;
    ops.statfs = op_statfs;
    ops.utimens = op_utimens;
    ops.chmod = op_chmod;
    ops.chown = op_chown;
    ops.getxattr = op_getxattr;
    ops.listxattr = op_listxattr;
#ifndef OFS_READONLY
    if (vol.caps & OFS_CAP_WRITE) {
        ops.setxattr = op_setxattr;
        ops.removexattr = op_removexattr;
        ops.write = op_write;
        ops.create = op_create;
        ops.mkdir = op_mkdir;
        ops.unlink = op_unlink;
        ops.rmdir = op_rmdir;
        ops.rename = op_rename;
        ops.truncate = op_truncate;
        ops.fsync = op_fsync;
    }
#endif
    fuse_opt_add_arg(&args, "-s");
    if (!(vol.caps & OFS_CAP_WRITE)) fuse_opt_add_arg(&args, "-oro");
    fuse_opt_add_arg(&args, "-ofsname=ofsfuse");
    r = fuse_main(args.argc, args.argv, &ops, NULL);
    fuse_opt_free_args(&args);
    return r;
}
