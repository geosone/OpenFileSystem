/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef OFS_ISO9660_H
#define OFS_ISO9660_H
/* ISO 9660 reading (ECMA-119), independent of I/O and of DOS, with the
   names of the Rock Ridge extensions (SUSP/RRIP: NM, with continuation
   areas, CE) or else of Joliet (the supplementary volume descriptor with
   UCS-2 names), else the plain ISO names without their ";1". Files of
   several extents (larger than 4 GiB) are read as one. Interleaved
   files are not read (OFS_ISO_EUNSUP).

   Logical blocks of 2048 bytes; the device's sectors may be 512 to 2048
   bytes (an image or a USB stick, a CD). There is no boot sector to probe:
   ofs_iso_mount looks for the volume descriptors at block 16. */
#include "ofs/codepage.h"

#define OFS_ISO_OK      0
#define OFS_ISO_END     1        /* end of directory */
#define OFS_ISO_EIO    -1        /* sector read failed */
#define OFS_ISO_EBAD   -2        /* not ISO 9660 or damaged */
#define OFS_ISO_ESIZE  -3        /* block or sector size not supported */
#define OFS_ISO_EUNSUP -6        /* an interleaved file */

/* attributes as DOS has them */
#define OFS_ISO_RDONLY 0x01
#define OFS_ISO_HIDDEN 0x02
#define OFS_ISO_DIR    0x10

#define OFS_ISO_BLOCK 2048
#define OFS_ISO_BUF (2 * OFS_ISO_BLOCK)   /* the buffer ofs_iso_mount wants */

typedef int (*ofs_iso_read_fn)(void *dev, ofs_lba lba, void OFS_FAR *buf);

struct ofs_iso {
    ofs_iso_read_fn read;
    void *dev;
    ofs_lba start;               /* first sector of the volume (device sectors) */
    ofs_u8 sshift;               /* device sector size, a shift (9-11) */
    ofs_u32 blocks;              /* volume space size, in blocks */
    ofs_u32 root_lba, root_size; /* the root directory of the tree in use */
    ofs_u8 joliet;               /* the Joliet tree is used */
    ofs_u8 rr;                   /* the primary tree has SUSP (Rock Ridge) */
    ofs_u8 skip;                 /* SUSP: bytes to skip in each system use area */
    ofs_u8 label_len;
    ofs_u16 label[32];
    const struct ofs_cp *cp;     /* the code page of plain ISO names (bytes above 7Fh) */
    ofs_s16 tz;                  /* minutes east of UTC of the DOS times: set after mount */
    /* the caller's buffer: a directory or data block, and a continuation area */
    ofs_u8 *buf, *ce;
    ofs_u32 buf_blk, ce_blk;     /* what they hold, or all ones */
};

/* A file or directory. Of a file of several extents only the first is
   here; the others follow its record in the directory (dir_lba,
   dir_size, rec). */
struct ofs_iso_file {
    ofs_u32 lba, len;            /* the (first) extent: block, bytes */
    ofs_fsize size;              /* all extents */
    ofs_u32 dir_lba, dir_size;   /* the directory of its record */
    ofs_u32 rec;                 /* the record's offset there */
    ofs_fsize ext_off;           /* the extent in ext_lba/ext_len starts here */
    ofs_u32 ext_lba, ext_len;
    ofs_u8 dir;                  /* a directory */
    ofs_u8 multi;                /* several extents */
    ofs_u8 unsup;                /* interleaved */
};

struct ofs_iso_ent {
    ofs_u32 pos;                 /* the offset of its record in the directory */
    ofs_u8 attr;
    ofs_u16 time, date;          /* DOS form, local (ofs_iso.tz) */
    struct ofs_iso_file file;
    ofs_u8 name_len;
    ofs_u16 name[255];
};

/* buf: OFS_ISO_BUF bytes. sshift: the device's sector size. */
int ofs_iso_mount(struct ofs_iso *, ofs_iso_read_fn, void *dev, ofs_lba start, unsigned sshift,
                  const struct ofs_cp *, ofs_u8 *buf);
void ofs_iso_rootdir(const struct ofs_iso *, struct ofs_iso_file *);
/* The next entry of a directory from *pos (0: the first), "." and ".."
   and Rock Ridge's relocated directories (RE) left out; *pos is the
   offset after the entry (and after the further extents of its file). */
int ofs_iso_dir_next(struct ofs_iso *, struct ofs_iso_file *dir, ofs_u32 *pos, struct ofs_iso_ent *);
int ofs_iso_read(struct ofs_iso *, struct ofs_iso_file *, ofs_fsize off, void OFS_FAR *buf,
                 ofs_u16 len, ofs_u16 *done);
#endif
