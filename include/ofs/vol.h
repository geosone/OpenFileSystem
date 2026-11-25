/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef OFS_VOL_H
#define OFS_VOL_H
/* One interface for every file system of OpenFileSystem (docs/DESIGN.md):
   a host gives its sector access once (struct ofs_host); ofs_mount finds
   the file system of a volume; files and directories are struct ofs_node,
   the same for FAT, exFAT and NTFS.  The frontends (FUSE, the DOS
   redirector, GonWare's volume layer) use only this, never a core.

   Which cores go in: OFS_NO_FAT, OFS_NO_EXFAT, OFS_NO_NTFS, OFS_NO_ISO, OFS_NO_UDF (all are in by
   default); OFS_READONLY leaves every write path out.  Calls on one volume
   come one at a time (the host serialises them). */
#include "ofs/types.h"
#include "ofs/codepage.h"
#ifndef OFS_NO_FAT
#include "ofs/fat.h"
#endif
#ifndef OFS_NO_EXFAT
#include "ofs/exfat.h"
#endif
#ifndef OFS_NO_NTFS
#include "ofs/ntfs.h"
#endif
#ifndef OFS_NO_ISO
#include "ofs/iso9660.h"
#endif
#ifndef OFS_NO_UDF
#include "ofs/udf.h"
#endif

/* results: the cores' own values where they agree, and more */
#define OFS_OK        0
#define OFS_END       1     /* end of a directory */
#define OFS_EIO      -1     /* a sector could not be read or written */
#define OFS_EBAD     -2     /* not a known file system, or damaged */
#define OFS_ESIZE    -3     /* sector, record or cluster size not supported */
#define OFS_EFULL    -4     /* no free space (or a file too large for it) */
#define OFS_EWRITE   -5     /* a sector write failed */
#define OFS_EEXIST   -6     /* the name exists */
#define OFS_ENAME    -7     /* not a valid name */
#define OFS_ENOENT  -20     /* no such name */
#define OFS_ENOTDIR -21
#define OFS_EISDIR  -22
#define OFS_EROFS   -23     /* the volume is read-only (or the file system) */
#define OFS_ENOTSUP -24     /* not supported (an encrypted NTFS file ...) */
#define OFS_ENOTEMPTY -25

/* attributes as DOS has them (every core uses these values) */
#define OFS_ATTR_RDONLY 0x01
#define OFS_ATTR_HIDDEN 0x02
#define OFS_ATTR_SYSTEM 0x04
#define OFS_ATTR_DIR    0x10
#define OFS_ATTR_ARCH   0x20

/* what a mounted volume can do: write into files; and with it change their
   sizes; create, remove and rename names (else OFS_ENOTSUP).  NTFS has
   only OFS_CAP_WRITE so far (stage 1: overwriting in place). */
#define OFS_CAP_WRITE  0x01
#define OFS_CAP_RESIZE 0x02
#define OFS_CAP_CREATE 0x04

/* The host: sector access and the clock. */
typedef int (*ofs_read_fn)(void *dev, ofs_lba lba, void OFS_FAR *buf);
typedef int (*ofs_write_fn)(void *dev, ofs_lba lba, const void OFS_FAR *buf);
struct ofs_host {
    ofs_read_fn read;             /* one sector at lba of the device: 0 or not */
    ofs_write_fn write;           /* NULL: the volume stays read-only */
    /* the local time as DOS keeps it, for new entries; NULL: 0 */
    void (*now)(void *dev, ofs_u16 *time, ofs_u16 *date);
    ofs_s16 tz;                   /* minutes east of UTC (NTFS keeps UTC) */
    ofs_u8 sshift;                /* the device's sector size, a shift; 0: 512 */
};

/* the buffer a volume needs (the largest of the cores built in) */
#if !defined(OFS_NO_UDF)
#define OFS_VOL_BUF OFS_UDF_BUF   /* 16 KiB, more than NTFS's */
#elif !defined(OFS_NO_NTFS)
#define OFS_VOL_BUF OFS_NTFS_BUF
#else
#define OFS_VOL_BUF 4096          /* also ISO 9660's OFS_ISO_BUF */
#endif

struct ofs_fs;

struct ofs_vol {
    const struct ofs_fs *fs;
    const char *type;             /* "FAT12", "FAT16", "FAT32", "exFAT", "NTFS", "ISO9660", "UDF" */
    struct ofs_host host;
    void *dev;
    ofs_u8 caps;                  /* OFS_CAP_* */
    ofs_u8 dirty;                 /* not cleanly unmounted (seen at mount) */
    char label[34];               /* UTF-8 */
    union {
#ifndef OFS_NO_FAT
        struct ofs_fat fat;
#endif
#ifndef OFS_NO_EXFAT
        struct ofs_exfat ex;
#endif
#ifndef OFS_NO_NTFS
        struct ofs_ntfs ntfs;
#endif
#ifndef OFS_NO_ISO
        struct ofs_iso iso;
#endif
#ifndef OFS_NO_UDF
        struct ofs_udf udf;
#endif
        ofs_u8 none;
    } u;
};

/* A file or directory. */
struct ofs_node {
    ofs_u8 dir;                   /* a directory */
    ofs_u8 attr;                  /* OFS_ATTR_* */
    ofs_fsize size;
    union {
#ifndef OFS_NO_FAT
        struct { struct ofs_fat_file f; struct ofs_fat_where w; } fat;
#endif
#ifndef OFS_NO_EXFAT
        struct { struct ofs_exfat_file f; struct ofs_exfat_where w; } ex;
#endif
#ifndef OFS_NO_NTFS
        struct ofs_ntfs_file ntfs;
#endif
#ifndef OFS_NO_ISO
        struct ofs_iso_file iso;
#endif
#ifndef OFS_NO_UDF
        struct ofs_udf_file udf;
#endif
        ofs_u8 none;
    } u;
};

/* A directory entry: its name, what it is, and the node to open it. */
struct ofs_dirent {
    ofs_u8 name_len;
    ofs_u16 name[255];            /* UTF-16 */
    ofs_u8 attr;
    ofs_u16 time, date;           /* last written, local DOS form */
    struct ofs_node node;
};

/* sector0 is the first sector of a volume (ofs_probe: 1 when a core with
   a boot sector knows it; ISO 9660 and UDF have none, ofs_mount tries them last); buf OFS_VOL_BUF bytes the host
   keeps for the volume; cp the code page of the 8.3 names.  The volume is
   writable when host->write is set and the file system can write (it is
   read-only, OFS_CAP_WRITE clear, otherwise). */
int ofs_probe(const ofs_u8 OFS_FAR *sector0);
int ofs_mount(struct ofs_vol *, const struct ofs_host *, void *dev, ofs_lba start,
              const struct ofs_cp *cp, ofs_u8 *buf);
int ofs_root(struct ofs_vol *, struct ofs_node *);
/* The next entry of dir from *pos (0: the first); OFS_END at the end. */
int ofs_next(struct ofs_vol *, struct ofs_node *dir, ofs_u32 *pos, struct ofs_dirent *);
/* The entry of a name (UTF-16), compared as the file system does
   (case-insensitive). */
int ofs_lookup(struct ofs_vol *, struct ofs_node *dir, const ofs_u16 *name, unsigned len,
               struct ofs_dirent *);
/* Reads up to len bytes at off; *done what was read (less at the end). */
int ofs_read(struct ofs_vol *, struct ofs_node *, ofs_fsize off, void OFS_FAR *buf,
             ofs_u32 len, ofs_u32 *done);
/* free clusters and the bytes of one */
int ofs_statfs(struct ofs_vol *, ofs_u32 *free_clusters, ofs_u32 *cluster_bytes,
               ofs_u32 *clusters);

#ifndef OFS_READONLY
/* Writing (OFS_EROFS on a read-only volume). */
int ofs_write(struct ofs_vol *, struct ofs_node *, ofs_fsize off, const void OFS_FAR *buf,
              ofs_u32 len);
int ofs_resize(struct ofs_vol *, struct ofs_node *, ofs_fsize size);
/* A new file, or with is_dir a directory, in dir; *out its entry. */
int ofs_create(struct ofs_vol *, struct ofs_node *dir, const ofs_u16 *name, unsigned len,
               int is_dir, struct ofs_dirent *out);
/* Removes the name from dir (a directory must be empty). */
int ofs_remove(struct ofs_vol *, struct ofs_node *dir, const ofs_u16 *name, unsigned len);
/* The name in dir becomes to_name in to (may be dir); to_name must not exist. */
int ofs_rename(struct ofs_vol *, struct ofs_node *dir, const ofs_u16 *name, unsigned len,
               struct ofs_node *to, const ofs_u16 *to_name, unsigned to_len);
/* What is written reaches the disk's structures (the dirty bit cleared). */
int ofs_sync(struct ofs_vol *);
#endif
#endif
