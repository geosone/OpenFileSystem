#ifndef OFS_EXFAT_DEFRAG_H
#define OFS_EXFAT_DEFRAG_H
/* exFAT and TexFAT defragmentation, the counterpart of fat_defrag.h: files
   and directories move whole through ofs_exfat_relocate (exfat.h), so the
   order of the writes is exfat.c's.  exFAT: the new clusters are marked,
   the data copied, linked, the entry set switched (one sector), the old
   clusters freed -- a power loss leaves at worst lost clusters, which the
   check repairs; VolumeDirty is set while it runs.  TexFAT: each move is a
   transaction (ActiveFat flips once it is complete), so a power loss leaves
   the volume as before the move or after it.  exFAT has no ".." entries;
   the root directory, the allocation bitmaps and the up-case table stay
   where they are.  Not moved (counted in stuck): files whose entry set
   would have to be switched with two sector writes outside a transaction.
   On TexFAT, a directory cluster copied by a transaction (TexFAT never
   changes a committed directory cluster in place) fragments its directory
   again; a last pass joins those directories, the deepest first.  The
   volume must be free of errors (exfat_check.c) and cleanly unmounted. */
#include "ofs/exfat.h"
#include "ofs/fat_defrag.h"            /* the modes, results and ofs_defrag_idx */

struct ofs_exdefrag {
    /* set by the caller */
    ofs_exfat_read_fn read;
    ofs_exfat_write_fn write;            /* not used by OFS_DEFRAG_ANALYZE */
    void *dev;
    ofs_lba start;                       /* first sector of the volume */
    int mode;                            /* OFS_DEFRAG_FULL, _FILES, _ANALYZE */
    /* work memory: one bit per cluster and 32 bytes per file; mem returns
       byte off, valid up to the end of its block (block: a power of two,
       at least 32) */
    void OFS_DEFRAG_FAR *(*mem)(void *ctx, ofs_u32 off);
    ofs_u32 memsize, block;
    ofs_defrag_idx OFS_DEFRAG_FAR *order; /* maxitems entries, for the files sorted by cluster */
    unsigned maxitems;
    ofs_u8 *buf;                         /* a sector buffer of OFS_EXFAT_MAXSECT bytes */
    void (*map)(void *ctx, ofs_u32 clu, ofs_u32 count, int state);   /* 0: no map */
    void (*status)(void *ctx, const char *text);
    int (*stop)(void *ctx);              /* nonzero: stop after the current move */
    void *ctx;
    /* results */
    unsigned tex;                        /* TexFAT */
    ofs_u32 clusters, cluster_size, used;
    ofs_u32 files, dirs;                 /* with clusters */
    ofs_u32 fragmented, fragmented_after;
    ofs_u32 stuck;                       /* fragmented files that could not be moved */
    ofs_u32 moves, moved;                /* files moved, clusters copied */
    ofs_u32 done;                        /* FULL: clusters 2 .. done-1 were placed */
};

int ofs_exdefrag_run(struct ofs_exdefrag *);
/* after ofs_exdefrag_run (also during, from the callbacks): cluster in use */
int ofs_exdefrag_used(ofs_u32 clu);
#endif
