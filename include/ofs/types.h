/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Fixed-width scalar types for host tests, 16-bit DOS builds (OpenWatcom)
 * and 32/64-bit kernels. */

#ifndef OFS_TYPES_H
#define OFS_TYPES_H

/* OpenWatcom's 16-bit DOS targets use 8/16/32-bit char/short/long. */
#ifdef __WATCOMC__
typedef unsigned char  ofs_u8;
typedef signed char    ofs_s8;
typedef unsigned short ofs_u16;
typedef signed short   ofs_s16;
typedef unsigned long  ofs_u32;
typedef signed long    ofs_s32;
typedef unsigned long long ofs_u64; /* OpenWatcom has it in every model */
typedef signed long long   ofs_s64;
#else
#include <stdint.h>
typedef uint8_t  ofs_u8;
typedef int8_t   ofs_s8;
typedef uint16_t ofs_u16;
typedef int16_t  ofs_s16;
typedef uint32_t ofs_u32;
typedef int32_t  ofs_s32;
typedef uint64_t ofs_u64; /* also under -std=c90: stdint.h declares it */
typedef int64_t  ofs_s64;
#endif

/* Sector numbers and file sizes: 64 bits in 32/64-bit builds, 32 bits
   under DOS (OpenWatcom, or OFS_NARROW), where neither disks past 2 TiB
   nor files of 4 GB and more are reachable and 64-bit arithmetic costs
   code in resident drivers. */
#if defined(__WATCOMC__) || defined(OFS_NARROW)
#define OFS_NARROW 1
typedef ofs_u32 ofs_lba;
typedef ofs_u32 ofs_fsize;
#else
typedef ofs_u64 ofs_lba;
typedef ofs_u64 ofs_fsize;
#endif

/* far data pointers of the 16-bit DOS memory models */
#ifdef __WATCOMC__
#define OFS_FAR __far
#else
#define OFS_FAR
#endif

#endif
