/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef OFS_LNK_H
#define OFS_LNK_H
/* Symbolic links on file systems without them (FAT, exFAT): a Windows
   shortcut name.lnk (MS-SHLLINK) in Cygwin's form, so that Cygwin takes
   it for the same symlink and Explorer follows it.  What makes one: the
   read-only attribute (the host checks it; not a directory), a 0x4C-byte
   header with only a description and a relative path (an item ID list
   allowed), shown normally; the description holds the target as written
   (or "[path too long]"), the relative path its Win32 form, and after
   both the full target as UTF-16 with a byte order mark.  Any other
   shortcut is a file. */
#include "ofs/types.h"

#define OFS_LNK_TARGET_MAX 1023          /* UTF-16 units of a target */
#define OFS_LNK_FILE_MAX   (0x4C + 3 * 2 + 2 * OFS_LNK_TARGET_MAX + 2 + 2 * OFS_LNK_TARGET_MAX)

/* 1 when the len bytes of buf are such a shortcut: *tlen units of its
   target into target (at most max); 0 when not one (or longer than max) */
int ofs_lnk_read(const ofs_u8 OFS_FAR *buf, ofs_u32 len, ofs_u16 *target, unsigned max, unsigned *tlen);
/* The shortcut of a target (UTF-16, tlen units, as written: "/" or "\")
   into buf: its length, 0 when size is too small or tlen 0 or too long */
ofs_u32 ofs_lnk_make(const ofs_u16 *target, unsigned tlen, ofs_u8 OFS_FAR *buf, ofs_u32 size);
#endif
