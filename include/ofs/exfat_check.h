#ifndef OFS_EXFAT_CHECK_H
#define OFS_EXFAT_CHECK_H
/* exFAT volume check for EXCHK, after the Microsoft exFAT file system
   specification: boot region and its backup, FAT, directory entry sets
   (checksums, name hashes, sizes), cluster chains against the allocation
   bitmap.  With a write function it also repairs what can be repaired.
   The clusters in use are kept in a bit window of the caller; a volume
   with more clusters than the window is checked in several passes. */
#include "ofs/exfat.h"

#define OFS_EXCHK_INFO  0
#define OFS_EXCHK_WARN  1
#define OFS_EXCHK_ERROR 2
#define OFS_EXCHK_FIXED 3                /* the message before has been repaired */

#define OFS_EXCHK_MAXSECT 4096

struct ofs_exchk {
    /* set by the caller */
    ofs_exfat_read_fn read;              /* sectors of the device */
    ofs_exfat_write_fn write;            /* 0: report only */
    void *dev;
    ofs_u32 start;                       /* first sector of the volume (the check
                                             reaches past 2^32 sectors from it in
                                             32/64-bit builds, 2^32 under DOS) */
    const struct ofs_cp *cp;       /* for the names in messages */
    void (*msg)(void *ctx, int level, const char *text);
    int (*mark)(void *ctx, ofs_u32 bit); /* sets a bit, returns the old one */
    void (*clear)(void *ctx);           /* clears all bits of the window */
    ofs_u32 window;                      /* bits of the window */
    void *ctx;
    /* results */
    ofs_u32 errors, warnings, fixed;     /* fixed: repairs written */
    ofs_u32 clusters, cluster_size;      /* of the volume */
    ofs_u32 used, free;                  /* by the allocation bitmap (after repairs) */
    ofs_u32 lost;                        /* allocated, but used by nothing */
    ofs_u32 unmarked;                    /* used, but free in the bitmap */
    ofs_u32 cross;                       /* used twice */
    ofs_u32 bad;                         /* marked bad in the FAT */
    ofs_u32 files, dirs;
    ofs_u16 flags;                       /* VolumeFlags: 2 dirty, 4 media failure */
    ofs_u32 serial;
};

/* 0: checked, the results tell; OFS_EXFAT_EBAD: no exFAT boot sector;
   OFS_EXFAT_EIO: the boot region cannot be read. */
int ofs_exchk_run(struct ofs_exchk *);
#endif
