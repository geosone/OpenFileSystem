/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef OFAT_NTFS_H
#define OFAT_NTFS_H
/* NTFS reading, independent of I/O and of DOS.  The on-disk structures as
   ntfs-3g (include/ntfs-3g/layout.h) describes them.  Read-only: MFT
   records with their update sequence fixups, resident and non-resident
   attributes (runlists, sparse runs), attribute lists, the $I30
   directory index ($INDEX_ROOT, $INDEX_ALLOCATION with its $BITMAP),
   compressed files (LZNT1, decompressed in idx 4 KB at a time).
   Encrypted files are not read (OFAT_NTFS_ECOMP).
   Sectors of 512 bytes only.  The interface follows exfat.h, so the DOS
   redirector of EXFAT.SYS can serve NTFS too. */
#include "openfat/codepage.h"

#define OFAT_NTFS_MAXREC 4096     /* MFT record and index block size */

#define OFAT_NTFS_OK      0
#define OFAT_NTFS_END     1       /* end of directory */
#define OFAT_NTFS_EIO    -1       /* sector read failed */
#define OFAT_NTFS_EBAD   -2       /* not NTFS or damaged */
#define OFAT_NTFS_ESIZE  -3       /* sector, record or index size not supported */
#define OFAT_NTFS_ECOMP  -6       /* encrypted data (or compressed other than LZNT1) */

/* attributes as DOS has them */
#define OFAT_NTFS_RDONLY 0x01
#define OFAT_NTFS_HIDDEN 0x02
#define OFAT_NTFS_SYSTEM 0x04
#define OFAT_NTFS_DIR    0x10
#define OFAT_NTFS_ARCH   0x20

#define OFAT_NTFS_ROOT   5        /* MFT record of the root directory */
#define OFAT_NTFS_MFTRUNS 16

typedef int (*ofat_ntfs_read_fn)(void *dev, ofat_lba lba, void OFAT_FAR *buf);

struct ofat_ntfs_run { ofat_u32 vcn, len, lcn; };   /* lcn all ones: sparse */

struct ofat_ntfs {
    ofat_ntfs_read_fn read;
    void *dev;
    ofat_lba start;               /* first sector of the volume */
    ofat_u8 cshift;               /* sectors per cluster, a shift */
    ofat_u16 rec_size, idx_size;  /* MFT record, index block: bytes */
    ofat_u32 clusters;            /* of the volume */
    ofat_u32 serial;
    ofat_u32 free;                /* free clusters, all ones: not counted */
    ofat_u8 label_len;
    ofat_u16 label[32];
    ofat_u8 dirty;                /* the volume's dirty flag (or a newer
                                     NTFS version: read anyway) */
    const struct ofat_cp *cp;
    ofat_s16 tz;                  /* minutes to add for the DOS times (NTFS
                                     has UTC): set after mount */
    /* runs of $MFT's data: the first ones, then the last looked up (a
       miss reads $MFT's runlist again) */
    ofat_u8 mft_nruns, mft_next, mft_busy;
    struct ofat_ntfs_run mft[OFAT_NTFS_MFTRUNS];
    /* buffers of the caller: a sector (512 bytes) and three of
       OFAT_NTFS_MAXREC: MFT records (two: a directory and its entries)
       and an index block */
    ofat_u8 *buf, *rec, *rec2, *idx;
    ofat_lba buf_lba;             /* sector in buf, or all ones */
    ofat_u32 rec_no, rec2_no, idx_no;   /* what they hold, or all ones */
    ofat_u32 idx_dir;             /* the directory of idx */
};

/* A file or directory: its MFT record, size, and the run last used. */
struct ofat_ntfs_file {
    ofat_u32 first;               /* MFT record number */
    ofat_fsize size, valid;       /* data size, initialized size */
    ofat_u32 idx, clu;            /* run cache: first VCN, LCN */
    ofat_u32 rlen;                /* its length in clusters, 0: none */
    ofat_u8 nofat;                /* 1 resident data, 2 encrypted, 3 compressed */
    ofat_u8 big;                  /* narrow: 4 GB or larger, cut */
    ofat_u8 root;                 /* a directory */
};

struct ofat_ntfs_ent {
    ofat_u32 pos;                 /* where the entry is (see dir_next) */
    ofat_u16 attr, time, date;
    ofat_u8 ns;                   /* name space: 0 POSIX, 1 Win32, 2 DOS, 3 both */
    struct ofat_ntfs_file file;
    ofat_u8 name_len;
    ofat_u16 name[255];
};

int ofat_ntfs_probe(const ofat_u8 OFAT_FAR *boot);
/* buf: OFAT_NTFS_MAXREC * 3 + 512 bytes */
int ofat_ntfs_mount(struct ofat_ntfs *, ofat_ntfs_read_fn, void *dev, ofat_lba start,
                    const struct ofat_cp *, ofat_u8 *buf);
void ofat_ntfs_rootdir(const struct ofat_ntfs *, struct ofat_ntfs_file *);
/* The next entry of a directory from *pos (0: the first): system files
   ($MFT ...) and DOS-only names (the 8.3 twin of a long name) are left out.
   *pos: block of the index (0 the root, n the n-th index block) << 16 |
   offset of the entry after it. */
int ofat_ntfs_dir_next(struct ofat_ntfs *, struct ofat_ntfs_file *dir, ofat_u32 *pos, struct ofat_ntfs_ent *);
int ofat_ntfs_read(struct ofat_ntfs *, struct ofat_ntfs_file *, ofat_fsize off, void OFAT_FAR *buf,
                   ofat_u16 len, ofat_u16 *done);
int ofat_ntfs_free(struct ofat_ntfs *, ofat_u32 *free_clusters);
/* 8.3 names (FCB form): the DOS name Windows gave it, else as exFAT does
   ("~n" for the n-th long name with the same basis). */
int ofat_ntfs_short(struct ofat_ntfs *, struct ofat_ntfs_file *dir, const struct ofat_ntfs_ent *, char fcb[11]);
int ofat_ntfs_lookup(struct ofat_ntfs *, struct ofat_ntfs_file *dir, const char fcb[11], struct ofat_ntfs_ent *);
#endif
