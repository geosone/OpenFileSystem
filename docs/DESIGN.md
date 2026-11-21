# OpenFileSystem: one core, many hosts

Status: agreed 2026-11-21, being implemented step by step (see Migration).

Decisions:
- **Names:** the whole C interface is `ofs_` / `OFS_` with headers in
  `include/ofs/`. The old `ofat_` / `OFAT_` / `openfat/` names are gone.
  There are no aliases: GonWare and OpenASPI follow when they move their
  submodule pin.
- **The DOS redirector moves here** (`hosts/dos`), out of OpenASPI.
- **A FUSE frontend** in `hosts/posix` tests the cores on Linux, through
  the same `ofs_vol` interface the DOS and GonWare frontends use, without
  booting either.

## Why

OpenFileSystem already does no I/O of its own, which is why OpenASPI's DOS
drivers and GonWare's kernel can share it. But every file system has its
own interface (`ofs_fat_*`, `ofs_exfat_*`, `ofs_ntfs_*`), and every
consumer writes the same glue for each of them again:

| Glue | DOS (OpenASPI) | GonWare | host tools |
| --- | --- | --- | --- |
| sector callbacks, buffers | `exfatsys.c`, `ntfssys.c` | `glm/fs/fat/{fat,exfat}.c`, `glm/fs/ntfs/ntfs.c` | `ofimg.c`, tests |
| lookup by name (fold, compare) | per driver | per driver | per test |
| reading in 64 KB pieces | per driver | per driver | |
| 8.3 names, times (UTC↔local) | per driver | per driver | |
| a redirector or VFS frontend | one per file system (EXFAT.SYS, NTFS.SYS) | one adapter per file system | |

A new file system (ISO 9660, UDF, NTFS writing) or a new limit (4 KB
sectors) has to be done N times. The aim: **a file system is written once
against one internal interface. A host is written once against one host
interface. Each host's frontend then serves every file system.**

That is the shape mlibc and uACPI have:

- **uACPI** defines a *kernel API* (`uacpi_kernel_*`) that the embedding OS
  implements outside the library. The library never knows the OS.
- **mlibc** keeps the OS-specific parts as *sysdeps* in its own tree
  (`sysdeps/<os>/`), chosen at build time. Each sysdep implements a fixed
  set of tagged functions (`Sysdeps<Read>` ...), and anything missing
  degrades to `ENOSYS`.

OpenFileSystem takes both: a uACPI-style host interface, so any OS can
embed it without upstreaming anything; and mlibc-style in-tree hosts for
the ones we maintain (DOS, GonWare, POSIX), so they are built and tested
together.

## Layers

```
 frontends (per host, written once)   DOS redirector | GonWare fs_driver | FUSE / ofimg
                                      ───────────────┴───────────┬───────┴─────────────
 ofs_vol API (uniform, per volume)    ofs_mount ofs_lookup ofs_readdir ofs_read ofs_write ...
                                      ───────────────────────────┬─────────────────────
 common helpers                       name fold/compare, 8.3, path walk, time, chunking
                                      ───────────────────────────┬─────────────────────
 file system cores (ofs_fs_ops)       fat      exfat      ntfs      (iso9660, udf ...)
                                      ───────────────────────────┬─────────────────────
 host interface (ofs_host_*)          sector I/O, memory, time, lock, log
                                      ───────────────────────────┴─────────────────────
 hosts/<host>/                        dos | gonware | posix
```

### 1. Host interface (`include/ofs/host.h`)

What the core needs from its host, all of it small. The **DOS host** works
without dynamic memory or 64-bit arithmetic, so everything that is optional
is a compile-time switch, not a requirement:

| Function | Needed for | DOS | GonWare | POSIX |
| --- | --- | --- | --- | --- |
| `ofs_host_read(dev, lba, count, buf)` / `_write` | everything, multi-sector | INT 13h / ASPI | `fs_disk_read` (OpenASPI) | `pread` |
| `ofs_host_sector_size(dev)` | 512 B *and* 4 KB sectors | 512 | `oa_disk.block_size` | image or ioctl |
| `ofs_host_alloc(size)` / `_free` | optional (`OFS_HAVE_ALLOC`) | none: caller buffers | `kmalloc` under the module's owner | `malloc` |
| `ofs_host_now(&utc)` | timestamps when writing | INT 21h + `/Z:` | `clock_now` | `time()` |
| `ofs_host_tz()` | DOS times ↔ UTC (NTFS) | `/Z:` option | `clock_zone_offset` | `localtime` |
| `ofs_host_lock(vol)` / `_unlock` | optional (`OFS_HAVE_LOCK`) | none (one task) | the volume layer's mutex | pthread |
| `ofs_host_log(level, msg)` | check/repair, debugging | to the screen | `kprintf` | stderr |

The *device* is opaque to the core (`void *dev`), as today. Today's
per-file-system callbacks (`ofs_*_read_fn`) map onto `ofs_host_read`
without changing a single call site.

### 2. Configuration (`include/ofs/config.h`, the uACPI `*_config.h` / mlibc options counterpart)

Selected per build, so a DOS driver links only what it uses:

- `OFS_NARROW`: 32-bit LBAs and sizes, as today.
- `OFS_FS_FAT`, `OFS_FS_EXFAT`, `OFS_FS_NTFS`, ...: which cores go in.
- `OFS_READONLY`: no write paths compiled in (NTFS.SYS today).
- `OFS_MAX_SECTOR`: 512 or 4096. This decides the static buffer sizes on
  DOS.
- `OFS_HAVE_ALLOC`, `OFS_HAVE_LOCK`: see above.

### 3. File system cores (`struct ofs_fs_ops`)

Each core keeps its current code. It also exports one operations table
that wraps its current functions:

```c
struct ofs_fs_ops {
    const char *name;                       /* "FAT32", "exFAT", "NTFS" */
    unsigned vol_size, node_size, buf_size; /* what the host must provide */
    unsigned caps;                          /* OFS_CAP_WRITE, _CHECK, _FORMAT, _SHORTNAMES, _LONGNAMES */
    int (*probe)(const ofs_u8 *sector0, unsigned sector_size);
    int (*mount)(struct ofs_vol *, void *buf);
    int (*root)(struct ofs_vol *, struct ofs_node *);
    int (*next)(struct ofs_vol *, struct ofs_node *dir, ofs_u32 *pos, struct ofs_dirent *);
    int (*stat)(struct ofs_vol *, struct ofs_dirent *);   /* NULL: next() fills everything */
    int (*read)(struct ofs_vol *, struct ofs_node *, ofs_fsize off, void OFS_FAR *, ofs_u16 len, ofs_u16 *done);
    /* NULL when the core (or the build) cannot write: */
    int (*write)(...), (*create)(...), (*remove)(...), (*rename)(...), (*resize)(...),
        (*set_attr)(...), (*set_time)(...), (*sync)(struct ofs_vol *);
    int (*statfs)(struct ofs_vol *, struct ofs_statfs *);
    int (*shortname)(...), (*lookup83)(...);          /* the DOS name space */
};
```

`struct ofs_vol` and `struct ofs_node` carry a common header (the ops, the
device, the sector size, the flags) followed by the core's private part.
The host reserves `vol_size` / `node_size` bytes, statically on DOS and in
a union or `kmalloc` in GonWare. Today GonWare hard-codes those unions in
`kernel/fs.h`; with the sizes from the ops table it no longer has to know
the file systems at all.

`struct ofs_dirent` is one form for every core:
- the name in UTF-16 (up to 255 units) and the 8.3 form;
- the attributes as DOS has them;
- the size and the valid length;
- times as UTC (a 64-bit count of 100 ns units). Each host converts these
  itself: to DOS date/time with `ofs_host_tz`, or to `timespec`.

### 4. Common helpers (`src/common/`)

Today each consumer writes these itself; here they exist once:

- `ofs_lookup(vol, dir, name16, len, &ent)`: case-insensitive through the
  volume's up-case rules. Those are exFAT's/NTFS's `$UpCase` where the core
  has one, and the code page where it has not. It uses the fast `next` +
  `stat` pair when the core offers it (NTFS `dir_name`).
- `ofs_walk(vol, path, &node)`: path walk, `\` and `/`.
- `ofs_read_all(vol, node, off, buf, len32, &done)`: the 64 KB chunking
  loop that every host repeats.
- 8.3: the `name83.c` / `ofs_cp_basis` logic, already shared.
- times: UTC ↔ DOS date/time with an offset.

### 5. Hosts (`hosts/<host>/`, the mlibc `sysdeps/<os>` counterpart)

| Host | What it contains | Built where |
| --- | --- | --- |
| `hosts/posix` | `ofs_host_*` over a file or block device; `ofimg`; the test programs; `ofsfuse`, a FUSE 3 frontend that mounts any volume the cores know (whole image or a partition of it), so Linux tools (`cp`, `diff`, `rsync`, fsck of the other side) test the cores | this repo's `make`, CI |
| `hosts/dos` | `ofs_host_*` over INT 13h / ASPI; **one** INT 2Fh redirector frontend over `ofs_vol` (today's exfatsys.c, generalised) | OpenWatcom, `make dos-check`. OpenASPI builds EXFAT.SYS and NTFS.SYS from it with `OFS_FS_*` chosen per driver, or one OFS.SYS for all |
| `hosts/gonware` | `ofs_host_*` over OpenASPI's native disk layer and the GDOS kernel facilities; **one** `fs_driver` adapter for every core | GonWare's CMake (`add_glm`): FAT.GLM and NTFS.GLM become a few lines each (the module header and `fs_register`) |

The DOS frontend (today's `exfatsys.c`/`ntfssys.c` with their assembler
frame) moves into `hosts/dos`. What it takes from OpenASPI (the BIOS/ASPI
disk access, the code page table loader, the volume table other drivers
read) becomes its `ofs_host_*` implementation or a small header that
OpenASPI includes back. Either way the frontend uses only `ofs_vol`, never a
core directly.

## What a new file system or a new limit costs then

- **A new file system** (ISO 9660, UDF): one core with an `ofs_fs_ops`
  table. DOS, GonWare and the tools serve it at once.
- **4 KB sectors**: the core reads `vol->sector_size` instead of assuming
  512. NTFS is the only core that still assumes 512 today. Every host
  benefits at once.
- **NTFS writing in stages**: `caps` grows step by step:
  1. overwrite in place, no size change;
  2. create and resize, with MFT and index updates and `$LogFile` reset;
  3. check.
  Each frontend enables a write operation the moment the table has it.
  No frontend changes.

## Migration (each step keeps every consumer working)

0. **Rename** `ofat_` → `ofs_`, `openfat/` → `ofs/` (done 2026-11-21).
   **Begun 2026-11-21:** `include/ofs/vol.h` + `src/vol.c`, the common
   interface over FAT, exFAT and NTFS, with a first, minimal `struct
   ofs_host` (one-sector read/write, the clock, the time zone; the
   allocation, lock and log hooks come with the hosts that need them), and
   `hosts/posix/ofsfuse.c` on it. `make fuse-check` (tests/fuse_host.sh):
   FAT12/16/32 and exFAT written through ofsfuse, compared with a
   reference tree, clean in fsck, and read back by the Linux drivers; NTFS
   (ntfs-3g's volume, compressed and sparse files) read through it.
   Times are kept in the DOS form with the host's offset for now, not yet
   as UTC.
   **Then (2026-11-21):** ISO 9660 as a fourth core (`src/iso9660/`, Rock
   Ridge, Joliet, several extents), the first without a boot sector:
   `struct ofs_fs` with probe NULL is tried by `ofs_mount` after the
   others, and `struct ofs_host` carries the device's sector size
   (`sshift`). Then UDF (`src/udf/`), also read-only for good: ISO and
   UDF media are made by burning software ported to the hosts, not by the
   cores. Both share `src/dostime.c` (a calendar time with a zone as the
   host's DOS time).
1. **Host interface and config**: add `ofs/host.h` and `ofs/config.h`.
   The existing `ofs_*_read_fn` callbacks are implemented on top of them;
   no call site changes. `hosts/posix` first: the tests and `ofimg` use it.
   `ofsfuse` read-only over FAT, exFAT and NTFS, then with writing for FAT
   and exFAT; the image tests compare it against the Linux drivers.
2. **Ops tables**: `ofs_fs_fat`, `ofs_fs_exfat`, `ofs_fs_ntfs` wrap the
   current functions. Add `struct ofs_dirent`. Add the common helpers.
   Tests run through both the old and the new interface.
3. **GonWare host**: one `fs_driver` adapter over `ofs_vol` replaces
   `glm/fs/fat/{fat,exfat}.c` and `glm/fs/ntfs/ntfs.c`. `kernel/fs.h` loses
   its per-file-system unions. Proof: GonWare's fs tests
   (`uefi-*-fs-*`, `-fs-ntfs`) unchanged and green.
4. **DOS host**: the redirector moves from OpenASPI to `hosts/dos` and works
   over `ofs_vol`; EXFAT.SYS and NTFS.SYS become builds of the same
   frontend. Proof: OpenASPI's DOS tests.
5. **4 KB sectors in NTFS**, then the NTFS write stages, done once in the
   core.
