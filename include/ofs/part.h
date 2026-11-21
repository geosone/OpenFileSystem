#ifndef OFS_PART_H
#define OFS_PART_H
/* Partition tables: MBR with extended partitions (the EBR chain) and GPT
   (UEFI specification, chapter 5), independent of I/O.  A disk whose first
   sector is a FAT or exFAT boot sector has no table ("superfloppy").
   GPT is used when the MBR has a protective entry (type EEh); a GPT with a
   damaged primary header or entry array is read through the backup
   header in the last sector. */
#include "ofs/types.h"

#define OFS_PART_NONE 0          /* no table: superfloppy or blank */
#define OFS_PART_MBR  1
#define OFS_PART_GPT  2

#define OFS_PART_EIO  -1         /* sector read failed */
#define OFS_PART_EBAD -2         /* damaged table */
#define OFS_PART_ESIZE -3        /* sector size not 512-4096 */

/* Reads one sector (1 << sshift bytes) at lba. */
typedef int (*ofs_part_read_fn)(void *dev, ofs_u64 lba, void OFS_FAR *buf);

struct ofs_part {
    ofs_u64 start, count;        /* in sectors */
    ofs_u32 index;               /* MBR: 1-4 primary, 5... logical; GPT: entry number from 1 */
    ofs_u8 scheme;               /* OFS_PART_MBR or OFS_PART_GPT */
    ofs_u8 type;                 /* MBR type; GPT: 0 */
    ofs_u8 active;               /* MBR: boot flag 80h */
    ofs_u8 type_guid[16], guid[16]; /* GPT, as stored on disk */
    ofs_u64 attr;                /* GPT attributes */
    ofs_u16 name[36];            /* GPT name, UTF-16, 0-terminated unless 36 long */
};

struct ofs_part_disk {
    ofs_u8 scheme;               /* OFS_PART_NONE, _MBR or _GPT */
    ofs_u8 backup;               /* GPT read through the backup header */
    ofs_u32 signature;           /* MBR disk signature (also on GPT disks) */
    ofs_u8 guid[16];             /* GPT disk GUID */
};

/* Called for each partition in table order: primary MBR partitions, then
   the logical ones; GPT entries in array order.  A positive return stops
   the scan (ofs_part_scan returns the partitions seen so far), a negative
   one stops it and is returned. */
typedef int (*ofs_part_fn)(void *ctx, const struct ofs_part *);

/* Reads the table of a disk of `sectors` sectors (0: unknown, then no
   backup GPT) with sectors of 1 << sshift bytes; buf holds one sector.
   Returns the number of partitions, or an error. */
int ofs_part_scan(ofs_part_read_fn, void *dev, unsigned sshift, ofs_u64 sectors,
                   ofs_u8 *buf, struct ofs_part_disk *, ofs_part_fn, void *ctx);

/* 1 when the sector looks like a FAT12/16/32 or exFAT boot sector. */
int ofs_part_is_vbr(const ofs_u8 OFS_FAR *sector0);

/* CRC-32 (IEEE 802.3, as in GPT and zip), continued from crc
   (start with 0). */
ofs_u32 ofs_crc32(ofs_u32 crc, const ofs_u8 OFS_FAR *p, unsigned len);
#endif
