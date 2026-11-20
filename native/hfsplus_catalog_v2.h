/* SPDX-License-Identifier: GPL-3.0-or-later
 * HFS+/HFSX catalog and fork decoder over authenticated Common V2 block I/O.
 * The same C implementation is used by hosted and UEFI builds.
 */
#ifndef IFS_HFSPLUS_CATALOG_V2_H
#define IFS_HFSPLUS_CATALOG_V2_H
#include <stdint.h>
#include <infiltratr/filesystem_module_v2.h>
typedef struct {
    uint32_t start, count;
} HfsPlusExtentV2;
typedef struct {
    uint64_t length;
    uint32_t total_blocks;
    HfsPlusExtentV2 extents[8];
} HfsPlusForkV2;
typedef struct {
    const IfsFsHostRuntimeV2 *host;
    uint32_t block_bytes, blocks, node_bytes, node_count, leaf_head;
    uint32_t variant;
    uint8_t case_sensitive;
    HfsPlusForkV2 catalog, bitmap_fork;
    uint8_t *bitmap;
    uint32_t bitmap_bytes;
} HfsPlusCatalogV2;
int hfsplus_catalog_open_v2(const IfsFsHostRuntimeV2 *, HfsPlusCatalogV2 *);
void hfsplus_catalog_close_v2(HfsPlusCatalogV2 *);
int hfsplus_catalog_stat_v2(const HfsPlusCatalogV2 *, const char *, IfsFsEntryV2 *);
int hfsplus_catalog_list_v2(const HfsPlusCatalogV2 *, const char *, uint32_t, IfsFsEntryV2 *);
int hfsplus_catalog_read_v2(const HfsPlusCatalogV2 *, const char *, uint64_t, void *, uint32_t);
#endif
