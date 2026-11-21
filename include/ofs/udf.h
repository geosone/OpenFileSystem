/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef OFS_UDF_H
#define OFS_UDF_H
/* UDF reading (ECMA-167 3rd edition, OSTA UDF 1.02 to 2.60), independent
   of I/O and of DOS. Read-only, as ISO 9660: discs and images are made by
   burning software.

   The anchor at block 256, the main volume descriptor sequence (partition
   and logical volume descriptors), partition maps of type 1, sparable
   (read as physical: a sparing table is not used) and metadata partitions
   (UDF 2.50+: the metadata file maps them), the file set descriptor, file
   entries and extended file entries with short, long and extended
   allocation descriptors (continued in allocation extent descriptors),
   data inside the ICB, unrecorded and unallocated extents (read as zeros),
   file identifiers in 8- and 16-bit OSTA compressed Unicode. Not read: a
   virtual partition (VAT, packet-written CD-R: OFS_UDF_EUNSUP at mount),
   ICB strategy 4096 (indirect entries: OFS_UDF_EUNSUP for that file).

   The logical block size is that of the anchor found: the device's sector
   size or larger, up to 4096. There is no boot sector to probe:
   ofs_udf_mount looks for the anchor. */
#include "ofs/codepage.h"

#define OFS_UDF_OK      0
#define OFS_UDF_END     1        /* end of directory */
#define OFS_UDF_EIO    -1        /* sector read failed */
#define OFS_UDF_EBAD   -2        /* not UDF or damaged */
#define OFS_UDF_ESIZE  -3        /* block size not supported */
#define OFS_UDF_EUNSUP -6        /* a virtual partition, an indirect entry */

#define OFS_UDF_RDONLY 0x01
#define OFS_UDF_HIDDEN 0x02
#define OFS_UDF_DIR    0x10

#define OFS_UDF_MAXBLOCK 4096
#define OFS_UDF_BUF (4 * OFS_UDF_MAXBLOCK)   /* the buffer ofs_udf_mount wants */
#define OFS_UDF_MAPS 4

typedef int (*ofs_udf_read_fn)(void *dev, ofs_lba lba, void OFS_FAR *buf);

/* A file or directory: its ICB (file entry) and the extent last used. */
struct ofs_udf_file {
    ofs_u32 icb_lb;              /* file entry: block in partition icb_part */
    ofs_u16 icb_part;            /* (an index into the partition maps) */
    ofs_u8 dir;
    ofs_u8 inl;                  /* the data is inside the file entry */
    ofs_u8 unsup;                /* an indirect entry */
    ofs_u8 ext_zero;             /* the cached extent reads as zeros */
    ofs_fsize size;
    ofs_fsize ext_off;           /* the cached extent: from this offset ... */
    ofs_u32 ext_abs, ext_len;    /* ... at this block of the volume, bytes */
};

struct ofs_udf_map {
    ofs_u8 meta;                 /* a metadata partition (else physical) */
    ofs_u16 pnum;                /* partition number */
    ofs_u32 start, len;          /* physical: blocks of the partition */
};

struct ofs_udf {
    ofs_udf_read_fn read;
    void *dev;
    ofs_lba start;               /* first sector of the volume (device sectors) */
    ofs_u8 sshift, bshift;       /* device sector, logical block: shifts */
    ofs_u8 nmaps;
    struct ofs_udf_map map[OFS_UDF_MAPS];
    struct ofs_udf_file meta;    /* the metadata file, when a map is one */
    struct ofs_udf_file root;
    ofs_u32 blocks;              /* of the (first) partition */
    ofs_u8 label_len;
    ofs_u16 label[63];
    const struct ofs_cp *cp;
    ofs_s16 tz;                  /* minutes east of UTC of the DOS times: set after mount */
    /* the caller's buffer: a file entry, the metadata file's entry, an
       allocation extent descriptor, a data block; what they hold, or all
       ones */
    ofs_u8 *fe, *mfe, *aed, *blk;
    ofs_u32 fe_blk, mfe_blk, aed_blk, blk_blk;
};

struct ofs_udf_ent {
    ofs_u32 pos;                 /* the offset of its identifier in the directory */
    ofs_u8 attr;
    ofs_u16 time, date;          /* last modified, DOS form, local (ofs_udf.tz) */
    struct ofs_udf_file file;
    ofs_u8 name_len;
    ofs_u16 name[255];
};

/* buf: OFS_UDF_BUF bytes. sshift: the device's sector size. */
int ofs_udf_mount(struct ofs_udf *, ofs_udf_read_fn, void *dev, ofs_lba start, unsigned sshift,
                  const struct ofs_cp *, ofs_u8 *buf);
void ofs_udf_rootdir(const struct ofs_udf *, struct ofs_udf_file *);
/* The next entry of a directory from *pos (0: the first): the parent and
   deleted identifiers left out. */
int ofs_udf_dir_next(struct ofs_udf *, struct ofs_udf_file *dir, ofs_u32 *pos, struct ofs_udf_ent *);
int ofs_udf_read(struct ofs_udf *, struct ofs_udf_file *, ofs_fsize off, void OFS_FAR *buf,
                 ofs_u16 len, ofs_u16 *done);
#endif
