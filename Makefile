# OpenFileSystem: host test programs, host tests and the OpenWatcom check.
CC      ?= cc
WATCOM  ?= /opt/watcom
WCC     ?= $(WATCOM)/binl64/wcc
BUILD   := build
CFLAGS  ?= -std=c90 -Wall -Wextra -Werror -pedantic -O2
CPPFLAGS += -Iinclude

HDR      := $(wildcard include/ofs/*.h)
CPSRC    := src/codepage.c src/latin.c src/exfat/upcase.c src/name83.c
FATSRC   := src/fat/fat.c src/fat/fat_ea.c $(CPSRC)
EXFAT    := src/exfat/exfat.c $(CPSRC)
# the UNI_<cp>.TAB files the tests load (mars-unicode-tables)
UNITAB   ?= ../mars-unicode-tables/TAB/unitables
CPPFLAGS += -DUNITAB=\"$(abspath $(UNITAB))\"
SRC      := $(wildcard src/*.c src/*/*.c)
H        := $(BUILD)/host

TESTBINS := $(H)/test_exfat $(H)/test_exfat_write $(H)/test_exchk $(H)/test_exfmt \
	$(H)/test_fatchk $(H)/test_defrag $(H)/test_fat_format $(H)/test_part $(H)/test_fat_write $(H)/test_exfat_big \
	$(H)/ofimg $(H)/test_ntfs $(H)/test_texfat_crash $(H)/test_exdefrag $(H)/test_recover $(H)/test_lnk $(H)/test_bmap

.PHONY: ntfs-check fuse fuse-check all test check dos-check clean
all: $(TESTBINS)

# quick tests without images
test: $(H)/test_fat_format $(H)/test_exfat $(H)/test_lnk
	$(H)/test_fat_format
	$(H)/test_exfat
	$(H)/test_lnk

# the host tests on images made by mkfs.fat/mkfs.exfat and the Linux drivers
check: all test dos-check
	bash tests/part_host.sh
	bash tests/fat_write.sh
	bash tests/fatchk_host.sh
	bash tests/defrag_host.sh
	bash tests/exdefrag_host.sh
	bash tests/recover_host.sh
	bash tests/exrecover_host.sh
	bash tests/exchk_host.sh
	bash tests/exfmt_host.sh
	bash tests/fatfmt_host.sh
	bash tests/exfat_write.sh
	$(H)/test_texfat_crash
	bash tests/exfat_big.sh
	bash tests/ofimg_host.sh
	bash tests/ntfs_host.sh
	sh tests/bmap_host.sh

# the longer NTFS tests: random trees through ntfs-3g in all cluster sizes
# (SEEDS from FIRST), a fragmented $MFT, and FUZZ damaged MFT records and
# index blocks under AddressSanitizer
FIRST ?= 1
SEEDS ?= 8
FUZZ  ?= 2000
ntfs-check: $(H)/test_ntfs $(H)/test_ntfs_asan $(H)/ofsfuse
	sh tests/ntfs_write.sh
	sh tests/ntfs_diff.sh $(FIRST) $(SEEDS)
	sh tests/ntfs_mftfrag.sh
	sh tests/ntfs_image.sh $(BUILD)/ntfs-fuzz 64 4096 >/dev/null
	python3 -I tests/ntfs_fuzz.py $(BUILD)/ntfs-fuzz/ntfs.img $(H)/test_ntfs_asan $(FIRST) $(FUZZ) $(BUILD)/ntfs-fuzz

$(H)/test_ntfs_asan: tests/ntfs_test.c src/ntfs/ntfs.c $(CPSRC) $(HDR)
	@mkdir -p $(H)
	$(CC) $(CPPFLAGS) -std=c90 -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer -o $@ $(filter %.c,$^)

$(H)/test_exfat: tests/exfat_test.c $(EXFAT) $(HDR)
$(H)/test_exfat_write: tests/exfat_write_test.c $(EXFAT) $(HDR)
$(H)/test_exfat_big: tests/exfat_big_test.c $(EXFAT) $(HDR)
$(H)/test_ntfs: tests/ntfs_test.c src/ntfs/ntfs.c $(CPSRC) $(HDR)
$(H)/test_exchk: tests/exchk_test.c src/exfat/exfat_check.c $(EXFAT) $(HDR)
$(H)/test_exfmt: tests/exfmt_test.c src/exfat/exfat_format.c src/exfat/upcase.c $(HDR)
$(H)/test_fatchk: tests/fatchk_test.c src/fat/fat_check.c $(CPSRC) $(HDR)
$(H)/test_defrag: tests/defrag_test.c src/fat/fat_defrag.c $(HDR)
$(H)/test_exdefrag: tests/exdefrag_test.c src/exfat/exfat_defrag.c $(EXFAT) $(HDR)
$(H)/test_recover: tests/recover_test.c src/fat/fat_recover.c src/exfat/exfat_recover.c $(HDR)
$(H)/test_fat_format: tests/fat_format_test.c src/fat/fat_format.c $(HDR)
$(H)/test_fat_write: tests/fat_write_test.c $(FATSRC) $(HDR)
$(H)/test_part: tests/part_test.c src/part/part.c $(HDR)
$(H)/test_lnk: tests/lnk_test.c src/lnk.c $(HDR)
# TexFAT transactions against power failures (through the common interface)
$(H)/test_texfat_crash: tests/texfat_crash_test.c src/vol.c src/exfat/exfat_check.c src/exfat/exfat_format.c $(EXFAT) $(HDR)
$(H)/test_texfat_crash: CPPFLAGS += -DOFS_NO_FAT -DOFS_NO_NTFS -DOFS_NO_ISO -DOFS_NO_UDF
# ofs_bmap of every core with sectors (FAT, exFAT, NTFS)
$(H)/test_bmap: tests/bmap_test.c src/vol.c src/fat/fat.c src/fat/fat_ea.c src/exfat/exfat.c src/ntfs/ntfs.c src/dostime.c $(CPSRC) $(HDR)
$(H)/test_bmap: CPPFLAGS += -DOFS_NO_ISO -DOFS_NO_UDF -DOFS_NTFS_MAXSECT=4096 -DOFS_FAT_MAXSECT=4096 -DOFS_EXFAT_MAXSECT=4096
# the host tool for images (tools/ofimg.c)
$(H)/ofimg: tools/ofimg.c src/exfat/exfat_format.c $(EXFAT) $(HDR)
$(H)/test_fat_write: CPPFLAGS += -DOFS_FAT_MAXSECT=4096
$(H)/test_exfat_big: CPPFLAGS += -DOFS_EXFAT_MAXSECT=4096
$(H)/test_ntfs $(H)/test_ntfs_asan $(H)/ofsfuse: CPPFLAGS += -DOFS_NTFS_MAXSECT=4096
# a host program with 64-bit printf (the library is checked as C90 elsewhere)
$(H)/test_exfat_big: CFLAGS := $(subst -std=c90,-std=c99,$(CFLAGS))
$(H)/test_recover: CFLAGS := $(subst -std=c90,-std=c99,$(CFLAGS))
$(TESTBINS):
	mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $(filter %.c,$^)

# ofsfuse (hosts/posix): every core through the common interface
# (src/vol.c), mounted with FUSE 3 -- for testing the cores on Linux
FUSESRC := hosts/posix/ofsfuse.c src/vol.c src/fat/fat.c src/fat/fat_ea.c src/exfat/exfat.c src/ntfs/ntfs.c src/iso9660/iso9660.c src/udf/udf.c src/dostime.c src/part/part.c $(CPSRC)
fuse: $(H)/ofsfuse
$(H)/ofsfuse: $(FUSESRC) $(HDR)
	mkdir -p $(@D)
	$(CC) $(CPPFLAGS) -std=gnu99 -Wall -Wextra -Werror -O2 $$(pkg-config --cflags fuse3) -o $@ \
		$(filter %.c,$^) $$(pkg-config --libs fuse3)
# the cores through ofsfuse against the Linux tools (mkfs, fsck, mtools,
# ntfs-3g): trees copied in and out, renamed, removed, compared
fuse-check: $(H)/ofsfuse
	bash tests/fuse_host.sh

# every source as a 16-bit DOS object (small model, 8086), so the library
# stays usable by the DOS drivers and tools
DOSOBJ := $(patsubst src/%.c,$(BUILD)/dos/%.obj,$(SRC))
dos-check: $(DOSOBJ)
$(BUILD)/dos/%.obj: src/%.c $(HDR)
	mkdir -p $(@D)
	WATCOM=$(WATCOM) $(WCC) -q -bt=dos -ms -0 -w4 -we -zq -os -s -iinclude -i$(WATCOM)/h -fo=$@ $<

clean:
	rm -rf $(BUILD)
