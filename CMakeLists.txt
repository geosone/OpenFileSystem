# SPDX-License-Identifier: GPL-2.0-or-later
#
# CMake build of OpenFileSystem for 32/64-bit hosts and kernels (GDOS). The host
# tests and the OpenWatcom check use the Makefile.
#
# OpenFileSystem::fat   reading and writing exFAT, formatting FAT12/16/32 and exFAT,
#                       reading NTFS
#                       (needs only string.h)
# OpenFileSystem::check checking/repairing and defragmenting (also needs stdio.h
#                       for its messages)
# As a subproject the parent sets the compiler and flags.

cmake_minimum_required(VERSION 3.20)
project(OpenFileSystem LANGUAGES C)

add_library(openfilesystem_fat STATIC
  src/exfat/exfat.c
  src/codepage.c
  src/latin.c
  src/name83.c
  src/exfat/exfat_format.c
  src/exfat/upcase.c
  src/fat/fat.c
  src/fat/fat_format.c
  src/part/part.c
  src/ntfs/ntfs.c)
target_include_directories(openfilesystem_fat PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
add_library(OpenFileSystem::fat ALIAS openfilesystem_fat)

add_library(openfilesystem_check STATIC
  src/exfat/exfat_check.c
  src/fat/fat_check.c
  src/fat/fat_defrag.c)
target_link_libraries(openfilesystem_check PUBLIC openfilesystem_fat)
add_library(OpenFileSystem::check ALIAS openfilesystem_check)

set_target_properties(openfilesystem_fat openfilesystem_check PROPERTIES
  C_STANDARD 90 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
