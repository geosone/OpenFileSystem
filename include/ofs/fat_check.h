#ifndef OFS_FAT_CHECK_H
#define OFS_FAT_CHECK_H
/* FAT12/16/32 volume check for ASPICHK: boot sector and BPB, the FAT32
   backup boot sector and FSInfo, the FAT copies against each other,
   directory entries and VFAT long names, cluster chains (free or bad
   clusters in them, loops, length against the size), cross-linked and lost
   clusters.  The rules follow fsck.fat of dosfstools 4.2.  With a write
   function it also repairs; a FAT entry that is broken in the active FAT
   is taken from another copy when that one is valid.  The clusters in use
   are kept in a bit window of the caller; a volume with more clusters than
   the window is checked in several passes. */
#include "ofs/types.h"

#ifdef __WATCOMC__
#define OFS_FATCHK_FAR __far
#else
#define OFS_FATCHK_FAR
#endif

#define OFS_FATCHK_INFO  0
#define OFS_FATCHK_WARN  1
#define OFS_FATCHK_ERROR 2
#define OFS_FATCHK_FIXED 3               /* the message before has been repaired */

#define OFS_FATCHK_OK    0
#define OFS_FATCHK_EBAD  (-1)            /* no FAT boot sector */
#define OFS_FATCHK_EIO   (-2)            /* the boot sector cannot be read */

#define OFS_FATCHK_MAXSECT 4096

struct ofs_fatchk {
    /* set by the caller */
    int (*read)(void *dev, ofs_u32 lba, void OFS_FATCHK_FAR *buf);    /* one sector */
    int (*write)(void *dev, ofs_u32 lba, const void OFS_FATCHK_FAR *buf);  /* 0: report only */
    void *dev;
    ofs_u32 start;                       /* first sector of the volume */
    const ofs_u16 *cp;                   /* Unicode of the OEM characters 80h-FFh, for names; 0: '?' */
    void (*msg)(void *ctx, int level, const char *text);
    int (*mark)(void *ctx, ofs_u32 bit); /* sets a bit, returns the old one */
    void (*clear)(void *ctx);           /* clears all bits of the window */
    ofs_u32 window;                      /* bits of the window */
    void *ctx;
    /* results */
    ofs_u32 errors, warnings, fixed;     /* fixed: repairs written */
    unsigned bits;                      /* 12, 16 or 32 */
    ofs_u32 clusters, cluster_size;
    ofs_u32 used, free, bad;             /* by the FAT (after repairs) */
    ofs_u32 lost;                        /* allocated, but used by nothing */
    ofs_u32 cross;                       /* used twice */
    ofs_u32 files, dirs;
    ofs_u32 serial;
};

/* 0: checked, the results tell; OFS_FATCHK_EBAD, OFS_FATCHK_EIO. */
int ofs_fatchk_run(struct ofs_fatchk *);
#endif
