#ifndef OFS_EXFAT_FORMAT_H
#define OFS_EXFAT_FORMAT_H
/* exFAT formatting for EXFMT, after the Microsoft exFAT file system
   specification: boot region with its backup and checksum, FAT (two of
   them for TexFAT), allocation bitmap, up-case table and root directory
   with the volume label.  512-byte sectors only.  The caller reads the
   medium for bad clusters; they are marked in the FAT and the bitmap, and
   the system clusters go round them. */
#include "ofs/exfat.h"

#define OFS_EXFMT_UPCASE_LEN 5836
#define OFS_EXFMT_MINSECT    2048UL      /* 1 MB, the smallest volume */
#define OFS_EXFMT_MAXBAD     256

extern const ofs_u8 ofs_exfmt_upcase[OFS_EXFMT_UPCASE_LEN];

struct ofs_exfmt {
    /* set by the caller */
    ofs_u32 sectors;                     /* of the volume */
    ofs_u32 hidden;                      /* its first sector on the medium */
    ofs_u32 cluster;                     /* bytes per cluster, 0: by size as Windows does */
    ofs_u8 nfats;                        /* 1, or 2 for TexFAT */
    ofs_u32 serial;
    ofs_u16 label[11];                   /* UTF-16 */
    ofs_u8 label_len;
    ofs_u32 align;                       /* the heap on a multiple of this many sectors (a
                                            power of two, as mkfs.exfat's 1 MiB); 0: a cluster */
    /* by ofs_exfmt_layout */
    ofs_u8 cshift;                       /* sectors per cluster, as a shift */
    ofs_u32 fat_off, fat_len, heap;      /* sectors from the volume start */
    ofs_u32 clusters;
    ofs_u32 bitmap_len;                  /* bytes */
    ofs_u32 sys_clusters;                /* bitmaps, up-case table, root directory */
    ofs_u32 bitmap_first[2], upcase_first, root;  /* by ofs_exfmt_write */
};

/* Writes count sectors at lba of the volume (0 = its boot sector). */
typedef int (*ofs_exfmt_write_fn)(void *dev, ofs_u32 lba, unsigned count, const void OFS_FAR *buf);

/* 0, or -1: volume too small or too large, wrong cluster size or nfats. */
int ofs_exfmt_layout(struct ofs_exfmt *);
/* Writes the file system.  bad: the bad clusters, ascending; buf: nbuf
   sectors.  The main boot region is written last.  0, -1 a write error,
   -2 the system clusters do not fit between the bad clusters. */
int ofs_exfmt_write(struct ofs_exfmt *, ofs_exfmt_write_fn, void *dev,
                   const ofs_u32 *bad, unsigned nbad, ofs_u8 OFS_FAR *buf, unsigned nbuf);
#endif
