# OpenFileSystem

Portable FAT12, FAT16, FAT32 and exFAT code in C90, and reading NTFS. It builds with OpenWatcom
for 16-bit DOS and with Clang or GCC for 32/64-bit hosts and kernels. The
library does no I/O of its own: sectors come and go through callbacks of the
caller. That way one code base serves the DOS drivers and tools of
[OpenASPI](https://github.com/geosone/OpenASPI) and the GDOS kernel of
GonWare.

| Part | Files | Content |
| --- | --- | --- |
| FAT | `src/fat/fat.c` | FAT12/16/32 with VFAT long names: mounting, directories, reading, writing, creating (8.3 names after the rules of Novell NSS's DOS name space: NT case bits, the upper case form, or `NAME~1`…`~4`, then `NAxxxx~1`, with a long name), removing, renaming and moving, attributes, times; dirty bit, FSInfo, every FAT copy |
| Partitions | `src/part/part.c` | MBR with the EBR chain of logical partitions, GPT with CRC checks and the backup header, superfloppy detection |
| Code pages | `src/codepage.c`, `src/latin.c`, `src/name83.c` | OEM code pages loaded from the `UNI_<cp>.TAB` files of [mars-unicode-tables](https://gitea.disconnected-by-peer.at/mars_nwe/mars-unicode-tables) (`ofs_cp_load`), DOS upper case, 8.3 basis names (shared by FAT and exFAT) |
| exFAT | `src/exfat/exfat.c` | mounting, directories (8.3 names: the basis, else `~` and the entry number in base 36), reading, writing, creating, removing, renaming; TexFAT volumes (two FATs and bitmaps) are written in transactions: the changes go to the inactive copies, directory clusters are copied before they change (the first cluster of a directory is TexFAT padding), `ofs_exfat_clean` commits by flipping ActiveFat in one sector write -- a power failure leaves the volume as before or after the transaction (tests/texfat_crash_test.c fails the power after every single write); file data overwritten in place is not covered |
| exFAT format | `src/exfat/exfat_format.c`, `upcase.c` | layout, boot region, bitmap, up-case table, root directory; TexFAT, bad clusters |
| exFAT check | `src/exfat/exfat_check.c` | checking and repairing |
| FAT format | `src/fat/fat_format.c` | FAT12/16/32 layout, boot sector, FSInfo |
| FAT check | `src/fat/fat_check.c` | checking and repairing FAT12/16/32 after the rules of fsck.fat |
| FAT defrag | `src/fat/fat_defrag.c` | defragmenting FAT12/16/32, in a write order that survives power loss |
| FAT recovery | `src/fat/fat_recover.c` | the last attempt on a FAT12/16/32 volume the check cannot repair, reading only: the tree from the root (a FAT entry from the first copy that links it, a broken chain continued with the clusters after it), unreadable sectors as zeros, the data area searched for lost directories, unused chains as `.CHK` files; the files go to the caller (tests/recover_host.sh: wiped FATs, root, boot sector, unreadable sectors, lost chains) |
| exFAT recovery | `src/exfat/exfat_recover.c` | the same for exFAT and TexFAT (`ofs_exfatrec_run`, same interface): the backup boot region, entry sets by their checksum, NoFatChain files and directories as their contiguous clusters, bytes past the valid data length as zeros, a broken chain continued with the next cluster the bitmap marks used; lost directories by the entry sets that begin a cluster, unused bitmap runs as `.CHK` (tests/exrecover_host.sh) |
| exFAT defrag | `src/exfat/exfat_defrag.c` | defragmenting exFAT and TexFAT: files and directories move whole (`ofs_exfat_relocate`); exFAT in a write order that leaves at worst lost clusters, TexFAT one transaction per move (a power failure leaves it before or after the move); entry sets whose switch would take two sector writes outside a transaction stay; tests/exdefrag_host.sh cuts the power along the whole run. Under DOS only with `OFS_EXFAT_RELOCATE` |
| Image tool | `tools/ofimg.c` | host tool: formats an exFAT volume in a disk image (a partition or a superfloppy), makes directories and puts files, without mounting it (mtools knows no exFAT): `ofimg IMAGE START format SECTORS [LABEL] mkdir PATH put FILE PATH …` |
| Common interface | `src/vol.c`, `include/ofs/vol.h` | one interface over every core (`ofs_mount`, `ofs_next`, `ofs_lookup`, `ofs_read`, `ofs_write`, `ofs_create`, `ofs_remove`, `ofs_rename`, `ofs_resize`, `ofs_sync`): the host gives its sector access once (`struct ofs_host`), the frontends never touch a core; cores left out with `OFS_NO_FAT`/`OFS_NO_EXFAT`/`OFS_NO_NTFS`, every write path with `OFS_READONLY` (docs/DESIGN.md) |
| FUSE | `hosts/posix/ofsfuse.c` | `ofsfuse [-o part=N] [-o ro] [-o sector=512] [-o cp=850] IMAGE MOUNTPOINT`: a FAT, exFAT, NTFS, ISO 9660 or UDF volume of an image (the whole image or a partition) mounted through FUSE 3 over the common interface, to test the cores with Linux tools |
| NTFS | `src/ntfs/ntfs.c` | read-only: MFT records, runlists (fragmented and sparse files), attribute lists, directory indexes, 8.3 names (the DOS names Windows gave, else `~` and the record number in base 36), label, free space; not compressed or encrypted files; sectors of 512 bytes |
| ISO 9660 | `src/iso9660/iso9660.c` | read-only: Rock Ridge names (NM, continuation areas, relocated deep directories), else Joliet (also long names), else the plain names without `;1`; files of several extents (over 4 GiB) as one; label; device sectors of 512 to 2048 bytes (an image, a USB stick, a CD drive); no interleaved files |
| UDF | `src/udf/udf.c` | read-only (discs are made by burning software): UDF 1.02 to 2.60, physical, sparable and metadata partitions, file entries and extended file entries, short/long/extended allocation descriptors with continuation, data inside the ICB, unrecorded extents as zeros, 8- and 16-bit names; logical blocks of 512 to 4096 bytes; no virtual partition (VAT) |
| GonWare | `hosts/gonware/ofsgw.c` | one driver of GonWare's volume layer over the common interface, built per module with the cores it carries (FAT.GLM, NTFS.GLM) |

Sector numbers (`ofs_lba`) and file sizes (`ofs_fsize`) have 64 bits in
32/64-bit builds: exFAT volumes past 2 TiB, files of 4 GiB and more, FAT and
exFAT volumes anywhere on large disks. Under DOS (OpenWatcom, or
`OFS_NARROW`) they have 32 bits, which keeps the resident drivers small;
exFAT files of 4 GiB and more are then read-only. The check, format and
defragmentation code still reaches 2^32 sectors of a volume.

All names start with `ofs_` (`OFS_` for constants); `include/ofs/types.h`
gives the fixed-width types.

Planned: the tools OFORMAT, OFSCK and ODEFRAG for
every FAT variant.

## Building and testing

```sh
make            # host test programs and ofimg in build/host
make test       # quick tests
make dos-check  # every source as a 16-bit DOS object (OpenWatcom, WATCOM=/opt/watcom)
make check      # everything, including the image tests
make ntfs-check # NTFS against ntfs-3g (random trees, all cluster sizes), fuzzing
make fuse       # build/host/ofsfuse (needs FUSE 3: pkg-config fuse3)
make fuse-check # the cores through ofsfuse against mkfs, fsck, xorriso, mkisofs -udf, mkudffs and the Linux drivers (ISO_BIG=1: also a 4.5 GiB file on ISO 9660)
```

The image tests compare with what the Linux drivers read and write, and
with fsck.fat, fsck.exfat and sfdisk. They need mkfs.fat, fsck.fat, mkfs.exfat, fsck.exfat, mtools,
Python 3, sfdisk and udisksctl (they fill images through the Linux drivers without
root). The tests load the code pages from `UNITAB` (default
`../mars-unicode-tables/TAB/unitables`, a checkout next to this one).
`src/latin.c` is made by `tools/mklatin.py` from `UCD/UnicodeData.txt` of
the same repository.
For NTFS they also need ntfsprogs (mkntfs, ntfsinfo, ntfsfix), ntfs-3g with
FUSE, and setfattr.  ntfs-3g's layout.h is the reference for the NTFS
structures.  `make ntfs-check` fills volumes of every cluster size (512
bytes to 64 KB) with a random tree through ntfs-3g (`tests/ntfs_gen.py`:
deep and large directories, long names with umlauts, fragmented, sparse
and compressed files, hard links, streams, deleted files) and compares
names, sizes and CRC32 of every file (`SEEDS=n FIRST=n`; `KEEP=dir`
keeps failing images), builds a volume whose $MFT is in many pieces, and
reads damaged MFT records and index blocks under AddressSanitizer
(`FUZZ=n` cases): the reader must give an error, never crash or hang.

CMake (as a subproject) gives `OpenFileSystem::fat` (with NTFS reading) and
`OpenFileSystem::check`. The repository was called openfat until it read
more than FAT; since 2026-11-21 the C interface is named after the
repository too: `ofs_` (`OFS_` for constants), `include/ofs/`. The old
`ofat_`/`openfat/` names are gone. The design of the next steps (one host
interface, one interface for every file system, hosts for POSIX/FUSE, DOS
and GonWare) is in [docs/DESIGN.md](docs/DESIGN.md).

## History

The code started in OpenASPI (`common/exfat`, `common/fat`) and moved here
with its history.

## License

GPL-2.0-or-later, see [LICENSE](LICENSE).
