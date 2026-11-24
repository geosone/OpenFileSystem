#ifndef OFS_FAT_RECOVER_H
#define OFS_FAT_RECOVER_H
/* FAT12/16/32 and exFAT recovery for ORECOVER: the last attempt on a volume the
   check cannot repair.  It never writes to the volume; the files go to the
   caller (another drive).  What it takes:
   - the boot sector, else the FAT32 backup (sector 6), else (when the
     caller gives the volume's size) the geometry searched for: the FATs
     by their first two entries (F8 FF FF ...) and the distance between the
     copies, the clusters by the "." entries of the directories (cluster
     number and sector), FAT12 or FAT16 by the count of clusters, FAT32's
     root at cluster 2;
   - the directory tree from the root: long names (when their checksum
     fits) or 8.3 names, every file with its size;
   - a cluster's FAT entry from the first copy that links it (a cluster or
     the end of a chain), so a FAT that was wiped is replaced by another;
   - a chain that breaks before the file's size is reached (a free, bad or
     wrong entry, a cluster already taken) goes on with the clusters after
     the last one, as UNDELETE guesses; such files are counted in guessed;
   - unreadable sectors as zeros (counted in bad);
   - with OFS_FATREC_SCAN the whole data area searched for the first
     clusters of directories ("." and ".." entries) that the tree did not
     reach; each, with what is below it, goes to \LOST\DIRnnnnn;
   - with OFS_FATREC_CHAINS the chains in the FAT that nothing uses, as
     \LOST\FILEnnnnn.CHK;
   - with OFS_FATREC_DELETED (FAT only), after the tree, its deleted files
     and directories (E5h) as UNDELETE finds them: the first cluster from
     the entry, then the clusters after it that no FAT copy uses and
     nothing saved has taken; the long name from its parts (their order
     on the disk) and the first character of the 8.3 name by the
     checksum.  They go to \DELETED with their directory's path (mkdir
     must succeed for a directory that is there already); one whose first
     cluster is in use again is left out (overwritten).
   exFAT (ofs_exfatrec_run, src/exfat/exfat_recover.c) the same way: the
   main boot sector, else the backup boot region (sector 12); file entry
   sets whose checksum fits (one that does not is taken when its stream
   and name entries are there, with a warning); NoFatChain files and
   directories as their contiguous clusters; bytes past a file's valid
   data length as zeros.  Its directories have no "." entry, so the scan
   takes the clusters that begin with a whole file entry set of a right
   checksum, those reached from another such directory left out (they come
   with it).  CHAINS also saves the runs of clusters the allocation bitmap
   marks used that nothing reached.
   Paths handed to the caller are relative ("\DIR\FILE", UTF-8); names are
   made safe for FAT (control characters and \/:*?"<>| become _). */
#include "ofs/types.h"

#ifdef __WATCOMC__
#define OFS_FATREC_FAR __far
#else
#define OFS_FATREC_FAR
#endif

#define OFS_FATREC_INFO  0
#define OFS_FATREC_WARN  1
#define OFS_FATREC_ERROR 2

#define OFS_FATREC_OK    0
#define OFS_FATREC_EBAD  (-1)            /* no FAT boot sector, not even the backup */
#define OFS_FATREC_EIO   (-2)            /* the boot sector cannot be read */
#define OFS_FATREC_EMEM  (-3)            /* the work memory is too small */
#define OFS_FATREC_ESTOP (-4)            /* stopped by the caller */

#define OFS_FATREC_SCAN   1              /* search the data area for lost directories */
#define OFS_FATREC_CHAINS 2              /* save the chains nothing uses */
#define OFS_FATREC_DELETED 4             /* save the deleted files too (FAT), to \DELETED */

#define OFS_FATREC_MAXSECT 4096

struct ofs_fatrec {
    /* set by the caller */
    int (*read)(void *dev, ofs_lba lba, void OFS_FATREC_FAR *buf);    /* one sector */
    void *dev;
    ofs_lba start;                       /* first sector of the volume */
    ofs_u32 sectors;                     /* its size in sectors (0: unknown): with it a
                                            FAT volume without a boot sector is searched
                                            for (ofs_fatrec_run) */
    unsigned sector_size;                /* for that search (0: 512) */
    const ofs_u16 *cp;                   /* Unicode of the OEM characters 80h-FFh; 0: '_' */
    int flags;                           /* OFS_FATREC_SCAN, OFS_FATREC_CHAINS */
    /* the target: nonzero from any of them is an error the run counts in
       failed and goes on (a file that could not be made is skipped) */
    int (*mkdir)(void *ctx, const char *path);
    int (*create)(void *ctx, const char *path, ofs_fsize size, ofs_u16 time, ofs_u16 date);
    int (*write)(void *ctx, const void *buf, unsigned len);
    int (*close)(void *ctx);
    void (*msg)(void *ctx, int level, const char *text);
    int (*stop)(void *ctx);              /* nonzero: stop (0: never) */
    /* work memory: two bits per cluster, and 8 bytes per directory found
       by the scan; mem returns byte off, valid up to the end of its block
       (block: a power of two, at least 8) */
    void OFS_FATREC_FAR *(*mem)(void *ctx, ofs_u32 off);
    ofs_u32 memsize, block;
    ofs_u8 *buf;                         /* two sectors of OFS_FATREC_MAXSECT bytes */
    void *ctx;
    /* results */
    unsigned bits;                       /* 12, 16 or 32; 64 for exFAT */
    ofs_u32 clusters, cluster_size;
    ofs_u32 files, dirs, bytes_lo, bytes_hi; /* bytes: saved, 64 bits */
    ofs_u32 guessed;                     /* files whose chain was guessed */
    ofs_u32 bad;                         /* unreadable sectors (zeros) */
    ofs_u32 lost_dirs, lost_chains;      /* found by SCAN, CHAINS */
    ofs_u32 failed;                      /* errors of the target */
    ofs_u32 undeleted;                   /* deleted files and directories saved (DELETED) */
    ofs_u32 overwritten;                 /* deleted ones left out: their first cluster in use again */
    int from_backup;                     /* the boot sector was the backup */
    int texfat;                          /* exFAT with two FATs */
    int searched;                        /* no boot sector: the geometry was searched for */
};

/* 0: done (the results tell); OFS_FATREC_EBAD, _EIO, _EMEM, _ESTOP. */
int ofs_fatrec_run(struct ofs_fatrec *);
/* the same for exFAT; OFS_FATREC_EBAD: no exFAT boot sector */
int ofs_exfatrec_run(struct ofs_fatrec *);
#endif
