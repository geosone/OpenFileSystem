/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef OFS_NTFS_H
#define OFS_NTFS_H
/* NTFS, independent of I/O and of DOS.  The on-disk structures as
   ntfs-3g (include/ntfs-3g/layout.h) describes them.  Reading: MFT
   records with their update sequence fixups, resident and non-resident
   attributes (runlists, sparse runs), attribute lists, the $I30
   directory index ($INDEX_ROOT, $INDEX_ALLOCATION with its $BITMAP),
   compressed files (LZNT1, decompressed in idx 4 KB at a time).
   Encrypted files are not read (OFS_NTFS_ECOMP).  Writing in stages:
   overwriting in place, sizes and creating so far (ofs_ntfs_write,
   ofs_ntfs_resize and ofs_ntfs_create, see below).
   Device sectors of 512 bytes up to OFS_NTFS_MAXSECT (4096 for 4Kn
   disks).  The interface follows exfat.h, so the DOS
   redirector of EXFAT.SYS can serve NTFS too. */
#include "ofs/codepage.h"

#define OFS_NTFS_MAXREC 4096     /* MFT record and index block size */
#ifndef OFS_NTFS_MAXSECT
#define OFS_NTFS_MAXSECT 512     /* the largest device sector */
#endif
/* the buffer ofs_ntfs_mount wants */
#define OFS_NTFS_BUF (OFS_NTFS_MAXREC * 3 + OFS_NTFS_MAXSECT)
/* the largest index entry: its header, a $FILE_NAME of 255 characters, a child VCN */
#define OFS_NTFS_ENTMAX (16 + ((0x42 + 2 * 255 + 7) & ~7) + 8)

#define OFS_NTFS_OK      0
#define OFS_NTFS_END     1       /* end of directory */
#define OFS_NTFS_EIO    -1       /* sector read failed */
#define OFS_NTFS_EBAD   -2       /* not NTFS or damaged */
#define OFS_NTFS_ESIZE  -3       /* sector, record or index size not supported */
#define OFS_NTFS_ECOMP  -6       /* encrypted data (or compressed other than LZNT1) */
#define OFS_NTFS_EWRITE -7       /* sector write failed */
#define OFS_NTFS_EROFS  -8       /* not writable: no write function, or the volume
                                     is dirty, hibernated, of another version, or
                                     its $LogFile not clean; a system file */
#define OFS_NTFS_ENOSPACE -9     /* not yet: a hole, data in an extension record,
                                     a runlist larger than its record */
#define OFS_NTFS_EFULL  -10      /* no free cluster */
#define OFS_NTFS_EEXIST -11      /* the name exists (case-insensitively) */
#define OFS_NTFS_ENAME  -12      /* not a valid Win32 name */
#define OFS_NTFS_ENOATTR -26     /* no such extended attribute (as vol.h's) */
#define OFS_NTFS_ERANGE -27      /* a buffer too small for it */

/* attributes as DOS has them */
#define OFS_NTFS_RDONLY 0x01
#define OFS_NTFS_HIDDEN 0x02
#define OFS_NTFS_SYSTEM 0x04
#define OFS_NTFS_DIR    0x10
#define OFS_NTFS_ARCH   0x20

#define OFS_NTFS_ROOT   5        /* MFT record of the root directory */
#define OFS_NTFS_MFTRUNS 16

typedef int (*ofs_ntfs_read_fn)(void *dev, ofs_lba lba, void OFS_FAR *buf);
typedef int (*ofs_ntfs_write_fn)(void *dev, ofs_lba lba, const void OFS_FAR *buf);

struct ofs_ntfs_run { ofs_u32 vcn, len, lcn; };   /* lcn all ones: sparse */

struct ofs_ntfs {
    ofs_ntfs_read_fn read;
    ofs_ntfs_write_fn write;     /* set by ofs_ntfs_set_write, else 0 */
    void *dev;
    ofs_lba start;               /* first sector of the volume */
    ofs_u8 sshift;               /* device sector size, a shift (9-12) */
    ofs_u8 cshift;               /* 512-byte blocks per cluster, a shift */
    ofs_u16 rec_size, idx_size;  /* MFT record, index block: bytes */
    ofs_u32 clusters;            /* of the volume */
    ofs_u32 serial;
    ofs_u32 free;                /* free clusters, all ones: not counted */
    ofs_u8 label_len;
    ofs_u16 label[32];
    ofs_u8 dirty;                /* the volume's dirty flag (or a newer
                                     NTFS version: read anyway) */
    const struct ofs_cp *cp;
    ofs_s16 tz;                  /* minutes to add for the DOS times (NTFS
                                     has UTC): set after mount */
    /* runs of $MFT's data: the first ones, then the last looked up (a
       miss reads $MFT's runlist again) */
    ofs_u8 mft_nruns, mft_next, mft_busy;
    struct ofs_ntfs_run mft[OFS_NTFS_MFTRUNS];
    /* buffers of the caller: a device sector and three of
       OFS_NTFS_MAXREC: MFT records (two: a directory and its entries)
       and an index block */
    ofs_u8 *buf, *rec, *rec2, *idx;
    ofs_lba buf_lba;             /* device sector in buf, or all ones */
    ofs_u32 rec_no, rec2_no, idx_no;   /* what they hold, or all ones */
    ofs_u32 idx_dir;             /* the directory of idx */
    ofs_lba idx_lba[OFS_NTFS_MAXREC / 512];   /* the blocks of idx's index block */
    /* writing: the runs of $Bitmap (0: none known, no allocation), where
       the last allocation ended, $LogFile emptied */
    struct ofs_ntfs_run bmr[4];
    ofs_u8 bm_nruns, log_reset;
    ofs_u32 bm_hint;
#ifndef OFS_READONLY
    /* creating: the NTFS minor version, $MFTMirr (where, how many records
       it copies), the runs of $UpCase (0: none known, no creating), its
       first block (characters 0-255) and one more (up_blk, or all ones),
       the index entry going in and the middle one of a split */
    ofs_u8 ver_minor, mirr_n, up_nruns;
    ofs_u32 mirr_lcn;
    struct ofs_ntfs_run upr[4];
    ofs_u16 up_blk;
    ofs_u8 up0[512], up1[512];
    ofs_u8 ent[OFS_NTFS_ENTMAX], ment[OFS_NTFS_ENTMAX];
#endif
};

/* A file or directory: its MFT record, size, and the run last used. */
struct ofs_ntfs_file {
    ofs_u32 first;               /* MFT record number */
    ofs_fsize size, valid;       /* data size, initialized size */
    ofs_u32 idx, clu;            /* run cache: first VCN, LCN */
    ofs_u32 rlen;                /* its length in clusters, 0: none */
    ofs_u8 nofat;                /* 1 resident data, 2 encrypted, 3 compressed */
    ofs_u8 big;                  /* narrow: 4 GB or larger, cut */
    ofs_u8 root;                 /* a directory */
};

struct ofs_ntfs_ent {
    ofs_u32 pos;                 /* where the entry is (see dir_next) */
    ofs_u16 attr, time, date;
    ofs_u16 ctime, cdate, atime, adate;   /* created, last accessed ($STANDARD_INFORMATION) */
    ofs_u8 ctime_cs;             /* 10 ms units of ctime */
    ofs_u8 ns;                   /* name space: 0 POSIX, 1 Win32, 2 DOS, 3 both */
    struct ofs_ntfs_file file;
    ofs_u8 name_len;
    ofs_u16 name[255];
};

int ofs_ntfs_probe(const ofs_u8 OFS_FAR *boot);
/* buf: OFS_NTFS_BUF bytes. sshift: the device's sector size (0: 512);
   the boot sector's bytes per sector must match it. */
int ofs_ntfs_mount(struct ofs_ntfs *, ofs_ntfs_read_fn, void *dev, ofs_lba start,
                    unsigned sshift, const struct ofs_cp *, ofs_u8 *buf);
void ofs_ntfs_rootdir(const struct ofs_ntfs *, struct ofs_ntfs_file *);
/* The next entry of a directory from *pos (0: the first): system files
   ($MFT ...) and DOS-only names (the 8.3 twin of a long name) are left out.
   *pos: block of the index (0 the root, n the n-th index block) << 16 |
   offset of the entry after it. */
int ofs_ntfs_dir_next(struct ofs_ntfs *, struct ofs_ntfs_file *dir, ofs_u32 *pos, struct ofs_ntfs_ent *);
/* The same entries without reading their MFT records: the name, the record
   (file.first) and OFS_NTFS_DIR only, quicker for a search by name;
   ofs_ntfs_stat then adds attributes, times and size. */
int ofs_ntfs_dir_name(struct ofs_ntfs *, struct ofs_ntfs_file *dir, ofs_u32 *pos, struct ofs_ntfs_ent *);
int ofs_ntfs_stat(struct ofs_ntfs *, struct ofs_ntfs_ent *);
int ofs_ntfs_read(struct ofs_ntfs *, struct ofs_ntfs_file *, ofs_fsize off, void OFS_FAR *buf,
                   ofs_u16 len, ofs_u16 *done);
int ofs_ntfs_free(struct ofs_ntfs *, ofs_u32 *free_clusters);
/* 8.3 names (FCB form): the DOS name Windows gave it, else as exFAT does
   ("~n" for the n-th long name with the same basis). */
int ofs_ntfs_short(struct ofs_ntfs *, struct ofs_ntfs_file *dir, const struct ofs_ntfs_ent *, char fcb[11]);
int ofs_ntfs_lookup(struct ofs_ntfs *, struct ofs_ntfs_file *dir, const char fcb[11], struct ofs_ntfs_ent *);
/* Windows NT's extended attributes of f ($EA_INFORMATION, $EA): names
   1-255 bytes, compared without case, kept as given; at most 65535 bytes
   packed.  The value's length in *len, the value into buf unless size is
   0; ofs_ntfs_ea_list: the names, each with a NUL. */
int ofs_ntfs_ea_get(struct ofs_ntfs *, struct ofs_ntfs_file *f, const char *name, void OFS_FAR *buf,
                    ofs_u16 size, ofs_u16 *len);
int ofs_ntfs_ea_list(struct ofs_ntfs *, struct ofs_ntfs_file *f, char OFS_FAR *buf, ofs_u16 size, ofs_u16 *len);
#ifndef OFS_READONLY
/* Writing, stage 1: data overwritten where it is (no allocation, no size
   change).  ofs_ntfs_set_write allows it when the volume is NTFS 3.x,
   not dirty, its $LogFile clean and Windows not hibernated on it
   (hiberfil.sys); else OFS_NTFS_EROFS and the volume stays read-only. */
int ofs_ntfs_set_write(struct ofs_ntfs *, ofs_ntfs_write_fn);
/* len bytes at off, not in a compressed or encrypted file
   (OFS_NTFS_ECOMP) nor in a hole of a sparse file (OFS_NTFS_ENOSPACE);
   past the end the file grows first (ofs_ntfs_resize), past the
   initialized size the bytes up to off become zeros.  The file's
   $STANDARD_INFORMATION gets the time written (the DOS local time and
   date, date 0: unchanged) and the archive bit. */
int ofs_ntfs_write(struct ofs_ntfs *, struct ofs_ntfs_file *, ofs_fsize off, const void OFS_FAR *buf,
                   ofs_u16 len, ofs_u16 *done, ofs_u16 time, ofs_u16 date);
/* Stage 2a: the file's size (its $DATA one attribute in the base record):
   clusters from $Bitmap or back to it, resident data moved out when it no
   longer fits; the new bytes read as zeros.  OFS_NTFS_EFULL: no space,
   OFS_NTFS_ENOSPACE: not yet so (see above). */
int ofs_ntfs_resize(struct ofs_ntfs *, struct ofs_ntfs_file *, ofs_fsize size, ofs_u16 time, ofs_u16 date);
/* A time of f (OFS_NTFS_CREATED, _WRITTEN, _READ: their offsets in
   $STANDARD_INFORMATION) from a local DOS time and 10 ms units, or its
   attributes (OFS_NTFS_RDONLY, _HIDDEN, _SYSTEM, _ARCH; the others kept):
   in $STANDARD_INFORMATION and the directory entries that repeat it. */
#define OFS_NTFS_CREATED 0x00
#define OFS_NTFS_WRITTEN 0x08
#define OFS_NTFS_READ    0x18
int ofs_ntfs_set_time(struct ofs_ntfs *, struct ofs_ntfs_file *, unsigned which, ofs_u16 time, ofs_u16 date,
                      ofs_u8 cs10);
int ofs_ntfs_set_attr(struct ofs_ntfs *, struct ofs_ntfs_file *, ofs_u8 attr);
/* Creates an empty file (is_dir 0) or directory named name (len UTF-16
   units, a valid Win32 name: OFS_NTFS_ENAME else) in dir: a free MFT
   record ($MFT grows by 16 records when none is free) and an entry in
   dir's index, ordered by the volume's $UpCase (a full index node is
   split, a full root moves into an index block).  *pos: where the entry
   is, for ofs_ntfs_dir_next.  OFS_NTFS_EEXIST: the name is there
   (case-insensitively); OFS_NTFS_ENOSPACE (not yet): $MFT or the index in
   more than its base record, a non-resident index $BITMAP, index blocks
   smaller than 2 KB. */
int ofs_ntfs_create(struct ofs_ntfs *, struct ofs_ntfs_file *dir, const ofs_u16 *name, unsigned len, int is_dir,
                    ofs_u16 time, ofs_u16 date, ofs_u32 *pos);
/* 1 when the directory dir has no entries, 0 when it has, or an error. */
int ofs_ntfs_empty(struct ofs_ntfs *, struct ofs_ntfs_file *dir);
/* Removes the file or (empty: OFS_NTFS_EEXIST else) directory of MFT
   record no from dir: its names out of dir's index (an emptied index block
   freed, the tree kept whole), its clusters free in $Bitmap, the record
   free; dir's times set.  Renames it: its names out of dir, the new one
   (name, len: a valid Win32 name) into to's index and its record, the
   times and sizes kept; *pos where the entry is in to.  OFS_NTFS_ENOSPACE
   (not yet): a name in another directory (a hard link), an attribute
   list, an object ID or reparse point (their $Extend indexes). */
int ofs_ntfs_remove(struct ofs_ntfs *, struct ofs_ntfs_file *dir, ofs_u32 no, ofs_u16 time, ofs_u16 date);
int ofs_ntfs_rename(struct ofs_ntfs *, struct ofs_ntfs_file *dir, ofs_u32 no, struct ofs_ntfs_file *to,
                    const ofs_u16 *name, unsigned len, ofs_u16 time, ofs_u16 date, ofs_u32 *pos);
/* An extended attribute set (how: OFS_NTFS_EA_SET, _CREATE: EEXIST when it
   is there, _REPLACE: ENOATTR when not, _REMOVE: ENOATTR when not), in
   f's base record, resident as Windows keeps small ones; the record
   changed time (time, date) set.  OFS_NTFS_ENOSPACE (not yet): they do
   not fit in the record, or $EA is non-resident or in an extension
   record; OFS_NTFS_EFULL: more than 65535 bytes packed. */
#define OFS_NTFS_EA_SET     0
#define OFS_NTFS_EA_CREATE  1
#define OFS_NTFS_EA_REPLACE 2
#define OFS_NTFS_EA_REMOVE  3
int ofs_ntfs_ea_set(struct ofs_ntfs *, struct ofs_ntfs_file *f, const char *name, const void OFS_FAR *val,
                    ofs_u16 vlen, int how, ofs_u16 time, ofs_u16 date);
#endif
#endif
