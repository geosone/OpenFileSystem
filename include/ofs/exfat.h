#ifndef OFS_EXFAT_H
#define OFS_EXFAT_H
/* exFAT reading and writing, independent of I/O and of DOS: after the
   Microsoft exFAT file system specification.  Sectors come from callbacks;
   the DOS driver and the host tests give their own.  Writing keeps an order
   that leaves at worst lost clusters behind: the allocation bitmap first,
   then the FAT, the data, and the directory entry last. */
#include "ofs/codepage.h"


#ifndef OFS_EXFAT_MAXSECT
#define OFS_EXFAT_MAXSECT 512
#endif

#define OFS_EXFAT_OK      0
#define OFS_EXFAT_END     1      /* end of directory */
#define OFS_EXFAT_EIO    -1      /* sector read failed */
#define OFS_EXFAT_EBAD   -2      /* not exFAT or damaged */
#define OFS_EXFAT_ESIZE  -3      /* sector size larger than OFS_EXFAT_MAXSECT */
#define OFS_EXFAT_EFULL  -4      /* no free cluster */
#define OFS_EXFAT_EWRITE -5      /* sector write failed, or no write function */

#define OFS_EXFAT_RDONLY 0x01
#define OFS_EXFAT_HIDDEN 0x02
#define OFS_EXFAT_SYSTEM 0x04
#define OFS_EXFAT_DIR    0x10
#define OFS_EXFAT_ARCH   0x20


/* Reads one sector (of 1 << sshift bytes) at lba of the device. */
typedef int (*ofs_exfat_read_fn)(void *dev, ofs_lba lba, void OFS_FAR *buf);
/* Writes one sector at lba of the device. */
typedef int (*ofs_exfat_write_fn)(void *dev, ofs_lba lba, const void OFS_FAR *buf);

#define OFS_EXFAT_CHG 32          /* changed sectors a transaction tracks */

/* moving files for defragmentation (exfat_defrag.c): in native builds,
   under DOS only on request (it is not needed by the drivers) */
#if !defined(__WATCOMC__) || defined(OFS_EXFAT_RELOCATE)
#define OFS_EXFAT_HAS_RELOCATE 1
#endif

struct ofs_exfat {
    ofs_exfat_read_fn read;
    void *dev;
    ofs_lba start;               /* first sector of the volume on the device */
    ofs_u32 fat, heap, clusters, root, serial;
    ofs_u32 bitmap;              /* first cluster of the allocation bitmap */
    ofs_u16 flags;               /* VolumeFlags */
    ofs_u8 sshift, cshift;       /* bytes per sector, sectors per cluster */
    ofs_u8 label_len;
    ofs_u16 label[11];
    const struct ofs_cp *cp;
    ofs_lba buf_lba;             /* sector in buf, or all ones */
    ofs_u8 *buf;                 /* OFS_EXFAT_MAXSECT bytes; volumes may share
                                   one when they set buf_lba to 0xFFFFFFFF
                                   after another one used it */
    /* writing: set write after ofs_exfat_mount; 0 keeps the volume read-only */
    ofs_exfat_write_fn write;
    /* TexFAT (two FATs and bitmaps): changes are transactions. They go
       to the working copies only -- the inactive FAT and bitmap, made
       equal to the active ones at the first change after mount -- and
       directory clusters are copied before they change (never the first
       cluster of a directory: it is padding, TexFAT padding entries, so
       no entry that points to a directory changes). ofs_exfat_clean
       commits: ActiveFat flips in the boot sector, one sector write, and
       the changed sectors go to the new working copies. A crash before
       the flip leaves the volume as it was, one after it as it is now.
       Not covered: file data overwritten in place, entries in the first
       cluster of a directory made elsewhere, directories without a FAT
       chain (NoFatChain) made elsewhere. */
    ofs_u8 tex;                  /* TexFAT: transactions */
    ofs_u8 tx;                   /* a transaction is open */
    ofs_u8 synced;               /* the working copies equal the active ones */
    ofs_u8 nchg, chg_all;        /* sectors changed in the transaction (all: too many) */
    ofs_u32 fat_a, fat_w;        /* active and working FAT (fat is fat_w after the first change) */
    ofs_u32 bm_a, bm_w;          /* active and working bitmap (bitmap likewise) */
    ofs_u32 fat_len, bm_len;     /* sectors of a FAT, bytes of a bitmap */
    ofs_u32 chg[OFS_EXFAT_CHG];  /* the changed sectors: 80000000h a bitmap's, else a FAT's */
    ofs_u16 gen;                 /* raised when a directory cluster is copied:
                                    walk caches of an older one are stale */
    ofs_u32 upcase, upcase_len;  /* the up-case table, for name hashes */
    ofs_u32 hint;                /* the next free cluster is looked for here */
    ofs_u32 free;                /* free clusters, 0FFFFFFFFh: not counted */
    ofs_u8 dirty;                /* VolumeDirty: 1 set by us, 2 set at mount */
#ifdef OFS_EXFAT_HAS_RELOCATE
    /* TexFAT: the directory clusters ofs_exfat_relocate's last call
       copied before it changed them ({old, new}) */
    ofs_u8 ncow;
    ofs_u32 cow[2][2];
#endif
};

/* A file or directory: its cluster chain and the cluster last walked to. */
struct ofs_exfat_file {
    ofs_u32 first;
    ofs_fsize size, valid;       /* size and valid data length (cut at 4 GB
                                     when narrow) */
    ofs_u32 idx, clu;            /* cluster index and number last walked to */
    ofs_u8 nofat;                /* contiguous, no FAT chain */
    ofs_u8 big;                  /* narrow: 4 GB or larger, read-only */
    ofs_u8 root;                 /* root directory: size from the chain */
    ofs_u16 gen;                 /* of the walk cache (ofs_exfat.gen) */
};

/* Where the entry set of a file or directory is: the directory (as in
   struct ofs_exfat_file) and the offset in it; none for the root directory. */
struct ofs_exfat_where {
    ofs_u32 first, pos;
    ofs_fsize size;
    ofs_u8 nofat, none;
};

struct ofs_exfat_ent {
    ofs_u32 pos;                 /* byte offset of the file entry */
    ofs_u16 attr, time, date;
    ofs_u16 ctime, cdate, atime, adate;   /* created, last accessed */
    ofs_u8 ctime_cs;             /* 10 ms units of ctime */
    ofs_u8 eas;                  /* FAT32.IFS's EA mark (byte 6 of the file entry):
                                    40h EAs in "name EA. SF", 80h critical ones */
    struct ofs_exfat_file file;
    ofs_u8 name_len;
    ofs_u16 name[255];
};

int ofs_exfat_probe(const ofs_u8 OFS_FAR *boot);
int ofs_exfat_mount(struct ofs_exfat *, ofs_exfat_read_fn, void *dev, ofs_lba start, const struct ofs_cp *, ofs_u8 *buf);
void ofs_exfat_rootdir(const struct ofs_exfat *, struct ofs_exfat_file *);
int ofs_exfat_dir_next(struct ofs_exfat *, struct ofs_exfat_file *dir, ofs_u32 *pos, struct ofs_exfat_ent *);
int ofs_exfat_read(struct ofs_exfat *, struct ofs_exfat_file *, ofs_fsize off, void OFS_FAR *buf, ofs_u16 len, ofs_u16 *done);
int ofs_exfat_free(struct ofs_exfat *, ofs_u32 *free_clusters);
/* the device sector holding byte off of a file (as ofs_fat_bmap); below
   the valid data length, OFS_EXFAT_END otherwise */
int ofs_exfat_bmap(struct ofs_exfat *, struct ofs_exfat_file *, ofs_fsize off, ofs_lba *lba);

/* Writing.  dir is a directory as given by ofs_exfat_rootdir or by an entry
   (a FAT chained one with root set); dw the place of its own entry, which
   changes when the directory grows.  Names are UTF-16, 1-255 characters,
   checked by the caller.  Each call leaves the volume consistent;
   VolumeDirty stays set until ofs_exfat_clean.  On TexFAT the calls up to
   ofs_exfat_clean are one transaction and VolumeDirty is not set. */
void ofs_exfat_where_of(const struct ofs_exfat_file *dir, ofs_u32 pos, struct ofs_exfat_where *);
void ofs_exfat_where_dir(const struct ofs_exfat_where *, struct ofs_exfat_file *dir);
/* Writes len bytes at off, from valid on with zeros before off; the file
   grows.  *done may be less than len: OFS_EXFAT_EFULL. */
int ofs_exfat_write(struct ofs_exfat *, struct ofs_exfat_file *, const struct ofs_exfat_where *,
                   ofs_fsize off, const void OFS_FAR *buf, ofs_u16 len, ofs_u16 *done);
/* The size: shorter frees clusters, longer allocates them (read as zeros). */
int ofs_exfat_resize(struct ofs_exfat *, struct ofs_exfat_file *, const struct ofs_exfat_where *, ofs_fsize size);
/* A new empty file, or with OFS_EXFAT_DIR a directory with one cluster;
   *pos its entry set. */
int ofs_exfat_create(struct ofs_exfat *, struct ofs_exfat_file *dir, struct ofs_exfat_where *dw,
                    const ofs_u16 *name, unsigned len, ofs_u16 attr, ofs_u16 time, ofs_u16 date, ofs_u32 *pos);
/* Removes the entry set at pos and frees its clusters (a directory must
   be empty: ofs_exfat_empty). */
int ofs_exfat_remove(struct ofs_exfat *, struct ofs_exfat_file *dir, ofs_u32 pos);
/* 1: no entries in the directory, 0: some, or an error. */
int ofs_exfat_empty(struct ofs_exfat *, struct ofs_exfat_file *dir);
/* The entry set at pos gets a new name, in the directory to (may be dir). */
int ofs_exfat_rename(struct ofs_exfat *, struct ofs_exfat_file *dir, ofs_u32 pos,
                    struct ofs_exfat_file *to, struct ofs_exfat_where *tw,
                    const ofs_u16 *name, unsigned len, ofs_u32 *newpos);
/* Attributes (the directory bit is kept). */
int ofs_exfat_attr(struct ofs_exfat *, struct ofs_exfat_file *dir, ofs_u32 pos, ofs_u16 attr);
/* FAT32.IFS's EA mark of the file entry at pos (byte 6, reserved in the
   specification). */
int ofs_exfat_eamark(struct ofs_exfat *, struct ofs_exfat_file *dir, ofs_u32 pos, ofs_u8 mark);
/* A time stamp: OFS_EXFAT_CREATED, _MODIFIED or _ACCESSED; cs10 10 ms units. */
#define OFS_EXFAT_CREATED  8
#define OFS_EXFAT_MODIFIED 12
#define OFS_EXFAT_ACCESSED 16
int ofs_exfat_time(struct ofs_exfat *, struct ofs_exfat_file *dir, ofs_u32 pos, unsigned which,
                  ofs_u16 time, ofs_u16 date, ofs_u8 cs10);
/* VolumeDirty cleared (unless it was set before the mount), PercentInUse. */
int ofs_exfat_clean(struct ofs_exfat *);

#ifdef OFS_EXFAT_HAS_RELOCATE
/* Defragmentation.  ofs_exfat_used: the cluster is in use (the working
   bitmap).  ofs_exfat_next: its FAT entry.  ofs_exfat_movable: 0 when the
   entry set at pos of dir cannot be switched safely -- its file and stream
   entries in two sectors where the switch is not one transaction (exFAT,
   and on TexFAT an entry in the first cluster of its directory or in a
   directory without a FAT chain, which are written in place).
   ofs_exfat_relocate moves the clusters of the file or directory whose
   entry set is at pos of dir (first: its first cluster, as the caller saw
   it) to the free clusters from target on, contiguous: they are marked,
   the data copied, linked when it had a FAT chain (it keeps NoFatChain
   otherwise), the stream entry switched (one sector), the old clusters
   freed.  A power loss leaves at worst lost clusters (exFAT; VolumeDirty
   stays set until ofs_exfat_clean).  On TexFAT the move is one
   transaction, committed; with its entry written in place two: the new
   clusters first, the old ones freed after the switch.  v->cow lists the
   directory clusters TexFAT copied on the way.  OFS_EXFAT_EBAD: the set
   is not as expected; OFS_EXFAT_EFULL: a target cluster is in use. */
int ofs_exfat_used(struct ofs_exfat *, ofs_u32 clu, int *used);
int ofs_exfat_next(struct ofs_exfat *, ofs_u32 clu, ofs_u32 *next);
int ofs_exfat_movable(struct ofs_exfat *, const struct ofs_exfat_file *dir, ofs_u32 pos);
int ofs_exfat_relocate(struct ofs_exfat *, struct ofs_exfat_file *dir, ofs_u32 pos, ofs_u32 first, ofs_u32 target);
#endif

/* 8.3 names: fcb is the 11-character FCB form ("LETTER~1TXT").  Returns 1
   when the long name had to be changed (it then needs a "~n"). */
int ofs_cp_basis(const struct ofs_cp *, const ofs_u16 *name, unsigned len, char fcb[11]);
int ofs_exfat_short(struct ofs_exfat *, struct ofs_exfat_file *dir, const struct ofs_exfat_ent *, char fcb[11]);
int ofs_exfat_lookup(struct ofs_exfat *, struct ofs_exfat_file *dir, const char fcb[11], struct ofs_exfat_ent *);
#endif
