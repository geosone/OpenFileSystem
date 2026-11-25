#ifndef OFS_FAT_H
#define OFS_FAT_H
/* FAT12, FAT16 and FAT32 reading and writing with VFAT long names,
   independent of I/O and of DOS, after Microsoft's FAT specification
   (fatgen103).  Sectors come from callbacks.  Writing keeps an order that
   leaves at worst lost clusters behind: the FAT first, then the data, the
   directory entry last.  All FAT copies are kept equal (on FAT32 only the
   active one when mirroring is off).  FAT16 and FAT32 carry the "volume
   dirty" bit in FAT entry 1 while the volume is being changed. */
#include "ofs/codepage.h"

#ifndef OFS_FAT_MAXSECT
#define OFS_FAT_MAXSECT 512
#endif

#define OFS_FAT_OK      0
#define OFS_FAT_END     1      /* end of directory or of a chain */
#define OFS_FAT_EIO    -1      /* sector read failed */
#define OFS_FAT_EBAD   -2      /* not FAT or damaged */
#define OFS_FAT_ESIZE  -3      /* sector size larger than OFS_FAT_MAXSECT */
#define OFS_FAT_EFULL  -4      /* no free cluster, or the fixed root directory is full */
#define OFS_FAT_EWRITE -5      /* sector write failed, or no write function */
#define OFS_FAT_EEXIST -6      /* the name exists */
#define OFS_FAT_ENAME  -7      /* not a valid long name */
#define OFS_FAT_ENOTSUP -24    /* extended attributes on FAT32 */
#define OFS_FAT_ENOATTR -26    /* no such extended attribute */
#define OFS_FAT_ERANGE  -27    /* the buffer is too small for it */

#define OFS_FAT_RDONLY 0x01
#define OFS_FAT_HIDDEN 0x02
#define OFS_FAT_SYSTEM 0x04
#define OFS_FAT_VOLUME 0x08
#define OFS_FAT_DIR    0x10
#define OFS_FAT_ARCH   0x20

/* Reads or writes one sector (of 1 << sshift bytes) at lba of the device. */
typedef int (*ofs_fat_read_fn)(void *dev, ofs_lba lba, void OFS_FAR *buf);
typedef int (*ofs_fat_write_fn)(void *dev, ofs_lba lba, const void OFS_FAR *buf);

struct ofs_fat {
    ofs_fat_read_fn read;
    void *dev;
    ofs_lba start;             /* first sector of the volume on the device */
    ofs_u32 fat, fat_size;     /* first sector of FAT 0, sectors per FAT */
    ofs_u32 root;              /* FAT12/16: first sector of the root directory;
                                   FAT32: its first cluster */
    ofs_u32 heap;              /* first sector of cluster 2 */
    ofs_u32 clusters;          /* clusters 2 .. clusters + 1 */
    ofs_u32 fsinfo;            /* FAT32: sector of FSInfo, else 0 */
    ofs_u32 serial;
    ofs_u16 root_entries;      /* FAT12/16 */
    ofs_u8 bits;               /* 12, 16 or 32 */
    ofs_u8 sshift, cshift;     /* bytes per sector, sectors per cluster */
    ofs_u8 nfats, active;      /* copies; the one read */
    ofs_u8 mirror;             /* writes go to every copy */
    char label[11];
    const struct ofs_cp *cp;
    ofs_u32 buf_lba;           /* sector in buf, or 0xFFFFFFFF */
    ofs_u8 *buf;               /* OFS_FAT_MAXSECT bytes; volumes may share
                                   one when they set buf_lba to 0xFFFFFFFF
                                   after another one used it */
    /* writing: set write after ofs_fat_mount; 0 keeps the volume read-only */
    ofs_fat_write_fn write;
    ofs_u32 hint;              /* the next free cluster is looked for here */
    ofs_u32 free;              /* free clusters, 0FFFFFFFFh: not counted */
    ofs_u8 dirty;              /* 1 set by us, 2 set at mount */
};

/* A file or directory: its cluster chain and the cluster last walked to. */
struct ofs_fat_file {
    ofs_u32 first, size;
    ofs_u32 idx, clu;          /* cluster index and number last walked to;
                                   clu 0: none */
    ofs_u8 dir;                /* a directory: its size is its chain */
    ofs_u8 root;               /* the fixed root directory of FAT12/16 */
};

/* Where the entry of a file is: the first cluster of its directory (0 for
   the root directory) and the offset of its short entry; none for the
   root directory itself. */
struct ofs_fat_where {
    ofs_u32 dir, pos;
    ofs_u8 none;
};

struct ofs_fat_ent {
    ofs_u32 pos;               /* byte offset of the short entry */
    ofs_u32 lfn;               /* offset of the first entry of the set */
    ofs_u8 attr, ntcase;       /* ntcase: 08h base, 10h extension lower case */
    ofs_u16 ea;                /* FAT12/16: the OS/2 and NT extended attributes'
                                   handle (offset 14h), 0: none */
    ofs_u16 time, date;        /* last written */
    ofs_u16 ctime, cdate, adate;
    ofs_u8 ctime_cs;           /* 10 ms units of ctime */
    char fcb[11];               /* the short name */
    struct ofs_fat_file file;
    ofs_u8 name_len;
    ofs_u16 name[255];         /* the long name, else the short one */
};

/* 12, 16 or 32 when the boot sector is one of a FAT volume, else 0. */
int ofs_fat_probe(const ofs_u8 OFS_FAR *boot);
int ofs_fat_mount(struct ofs_fat *, ofs_fat_read_fn, void *dev, ofs_lba start,
                   const struct ofs_cp *, ofs_u8 *buf);
void ofs_fat_rootdir(const struct ofs_fat *, struct ofs_fat_file *);
/* The directory whose first cluster is first (0: the root directory). */
void ofs_fat_dirfile(const struct ofs_fat *, ofs_u32 first, struct ofs_fat_file *);
/* The next entry from *pos on ("." and "..", volume labels and deleted
   entries are skipped); OFS_FAT_END at the end. */
int ofs_fat_dir_next(struct ofs_fat *, struct ofs_fat_file *dir, ofs_u32 *pos, struct ofs_fat_ent *);
/* The entry of a long or short name, case-insensitive. */
int ofs_fat_find(struct ofs_fat *, struct ofs_fat_file *dir, const ofs_u16 *name, unsigned len,
                  struct ofs_fat_ent *);
/* The entry with the short name fcb (11 characters, FCB form). */
int ofs_fat_lookup(struct ofs_fat *, struct ofs_fat_file *dir, const char fcb[11], struct ofs_fat_ent *);
int ofs_fat_read(struct ofs_fat *, struct ofs_fat_file *, ofs_u32 off, void OFS_FAR *buf,
                  ofs_u16 len, ofs_u16 *done);
int ofs_fat_free(struct ofs_fat *, ofs_u32 *free_clusters);

/* Writing.  Each call leaves the volume consistent; the dirty bit stays set
   until ofs_fat_clean. */
void ofs_fat_where_of(const struct ofs_fat_file *dir, ofs_u32 pos, struct ofs_fat_where *);
/* Writes len bytes at off; the file grows, with zeros between its end and
   off.  *done may be less than len: OFS_FAT_EFULL. */
int ofs_fat_write(struct ofs_fat *, struct ofs_fat_file *, const struct ofs_fat_where *,
                   ofs_u32 off, const void OFS_FAR *buf, ofs_u16 len, ofs_u16 *done);
/* The size: shorter frees clusters, longer adds zeros. */
int ofs_fat_resize(struct ofs_fat *, struct ofs_fat_file *, const struct ofs_fat_where *, ofs_u32 size);
/* A new empty file, or with OFS_FAT_DIR a directory with "." and "..";
   names are UTF-16, 1-255 characters.  The short name is the name itself
   when it is a valid 8.3 name (lower case through the NT case bits), else
   a basis name with "~n" and the long name in VFAT entries.  *pos: the
   short entry. */
int ofs_fat_create(struct ofs_fat *, struct ofs_fat_file *dir, const ofs_u16 *name, unsigned len,
                    ofs_u8 attr, ofs_u16 time, ofs_u16 date, ofs_u32 *pos);
/* Removes the entry at pos and frees its clusters (a directory must be
   empty: ofs_fat_empty). */
int ofs_fat_remove(struct ofs_fat *, struct ofs_fat_file *dir, ofs_u32 pos);
/* 1: no entries in the directory, 0: some, or an error. */
int ofs_fat_empty(struct ofs_fat *, struct ofs_fat_file *dir);
/* The entry at pos gets a new name, in the directory to (may be dir); a
   moved directory's ".." follows. */
int ofs_fat_rename(struct ofs_fat *, struct ofs_fat_file *dir, ofs_u32 pos,
                    struct ofs_fat_file *to, const ofs_u16 *name, unsigned len, ofs_u32 *newpos);
/* Attributes (the directory and volume bits are kept). */
int ofs_fat_attr(struct ofs_fat *, struct ofs_fat_file *dir, ofs_u32 pos, ofs_u8 attr);
/* A time stamp: OFS_FAT_CREATED, _MODIFIED or _ACCESSED (date only). */
#define OFS_FAT_CREATED  14
#define OFS_FAT_MODIFIED 22
#define OFS_FAT_ACCESSED 18
int ofs_fat_time(struct ofs_fat *, struct ofs_fat_file *dir, ofs_u32 pos, unsigned which,
                  ofs_u16 time, ofs_u16 date, ofs_u8 cs10);
/* The entry whose short entry is at pos. */
int ofs_fat_entry_at(struct ofs_fat *, struct ofs_fat_file *dir, ofs_u32 pos, struct ofs_fat_ent *);
/* A file with exactly the short name fcb and no long name (as "EA DATA. SF",
   whose space no long name would keep). */
int ofs_fat_create_fcb(struct ofs_fat *, struct ofs_fat_file *dir, const char fcb[11], ofs_u8 attr,
                        ofs_u16 time, ofs_u16 date, ofs_u32 *pos);
/* FAT12/16: the extended attributes' handle of the entry at pos. */
int ofs_fat_set_ea(struct ofs_fat *, struct ofs_fat_file *dir, ofs_u32 pos, ofs_u16 handle);

/* OS/2's and Windows NT's extended attributes on FAT12/16 (src/fat/fat_ea.c):
   one hidden file "EA DATA. SF" in the root directory holds every file's
   set, the directory entry the set's handle (offset 14h, which FAT32 needs
   for the cluster number: OFS_FAT_ENOTSUP there).  Names are 1-255 bytes,
   compared without case; a set holds at most 65535 bytes.  e is the entry
   (ofs_fat_entry_at, ofs_fat_find); time and date stamp a new EA file.
   Changing a set is safe against power loss: the new set is appended, its
   handle then points to it (one write), the old one is squeezed out last;
   what a crash leaves behind the next change squeezes out. */
#define OFS_FAT_EA_SET     0   /* create or replace */
#define OFS_FAT_EA_CREATE  1   /* OFS_FAT_EEXIST when it exists */
#define OFS_FAT_EA_REPLACE 2   /* OFS_FAT_ENOATTR when it does not */
#define OFS_FAT_EA_REMOVE  3
/* The value's length in *len, the value into buf when size is not 0. */
int ofs_fat_ea_get(struct ofs_fat *, const struct ofs_fat_ent *e, const char *name, void OFS_FAR *buf,
                    ofs_u16 size, ofs_u16 *len);
/* The names, each with a NUL, into buf (size 0: only their length). */
int ofs_fat_ea_list(struct ofs_fat *, const struct ofs_fat_ent *e, char OFS_FAR *buf, ofs_u16 size,
                     ofs_u16 *len);
int ofs_fat_ea_set(struct ofs_fat *, struct ofs_fat_file *dir, const struct ofs_fat_ent *e, const char *name,
                    const void OFS_FAR *val, ofs_u16 vlen, int how, ofs_u16 time, ofs_u16 date);
/* A removed file's set (ofs_fat_remove does it). */
int ofs_fat_ea_drop(struct ofs_fat *, ofs_u16 handle);

/* The dirty bit cleared (unless it was set before the mount), FSInfo. */
int ofs_fat_clean(struct ofs_fat *);
#endif
