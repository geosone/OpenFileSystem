/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef OFS_NTFS_H
#define OFS_NTFS_H
/* NTFS reading, independent of I/O and of DOS.  The on-disk structures as
   ntfs-3g (include/ntfs-3g/layout.h) describes them.  Read-only: MFT
   records with their update sequence fixups, resident and non-resident
   attributes (runlists, sparse runs), attribute lists, the $I30
   directory index ($INDEX_ROOT, $INDEX_ALLOCATION with its $BITMAP),
   compressed files (LZNT1, decompressed in idx 4 KB at a time).
   Encrypted files are not read (OFS_NTFS_ECOMP).
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

#define OFS_NTFS_OK      0
#define OFS_NTFS_END     1       /* end of directory */
#define OFS_NTFS_EIO    -1       /* sector read failed */
#define OFS_NTFS_EBAD   -2       /* not NTFS or damaged */
#define OFS_NTFS_ESIZE  -3       /* sector, record or index size not supported */
#define OFS_NTFS_ECOMP  -6       /* encrypted data (or compressed other than LZNT1) */

/* attributes as DOS has them */
#define OFS_NTFS_RDONLY 0x01
#define OFS_NTFS_HIDDEN 0x02
#define OFS_NTFS_SYSTEM 0x04
#define OFS_NTFS_DIR    0x10
#define OFS_NTFS_ARCH   0x20

#define OFS_NTFS_ROOT   5        /* MFT record of the root directory */
#define OFS_NTFS_MFTRUNS 16

typedef int (*ofs_ntfs_read_fn)(void *dev, ofs_lba lba, void OFS_FAR *buf);

struct ofs_ntfs_run { ofs_u32 vcn, len, lcn; };   /* lcn all ones: sparse */

struct ofs_ntfs {
    ofs_ntfs_read_fn read;
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
#endif
