# SPDX-License-Identifier: GPL-3.0-or-later
# The same format-owned HFS+ source compiles as hosted and firmware modules.
cmake_minimum_required(VERSION 3.20)
project(HFSPlusFirmware LANGUAGES C)
set(COMMON_INCLUDE_DIR "" CACHE PATH "Pinned Common V2 ABI")
if(NOT EXISTS "${COMMON_INCLUDE_DIR}/infiltratr/filesystem_module_v2.h")
  message(FATAL_ERROR "Pinned Common V2 header required")
endif()
add_library(hfsplus-firmware MODULE
  ../../native/hfsplus_catalog_v2.c
  ../../module/hfsplus_module.c
  runtime.c)
set_target_properties(hfsplus-firmware PROPERTIES
  PREFIX "" OUTPUT_NAME "HFSPLUS" SUFFIX ".so" C_VISIBILITY_PRESET hidden)
target_include_directories(hfsplus-firmware PRIVATE
  "${COMMON_INCLUDE_DIR}"
  "${CMAKE_CURRENT_SOURCE_DIR}/../../native")
target_compile_features(hfsplus-firmware PRIVATE c_std_11)
target_compile_definitions(hfsplus-firmware PRIVATE IFS_HFSPLUS_FIRMWARE_BUILD=1)
# Firmware uses AMD64 SysV without CET, SIMD state assumptions,
# packed relative relocations or non-4-KiB load alignment. Hosted builds
# retain their separate native optimization profile.
target_compile_options(hfsplus-firmware PRIVATE
  -O2 -Wall -Wextra -Wpedantic -Werror
  -ffreestanding -fno-builtin -fno-stack-protector
  -ffunction-sections -fdata-sections -mno-red-zone -fcf-protection=none -mgeneral-regs-only
  -fno-tree-loop-distribute-patterns -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0)
target_link_options(hfsplus-firmware PRIVATE
  -nostdlib -Wl,-z,nopack-relative-relocs,-z,max-page-size=4096,-z,defs,-z,noexecstack,-z,relro,-z,now,--gc-sections,-Bsymbolic)
