/* SPDX-License-Identifier: GPL-3.0-or-later
 * Source-owned HFS+ / HFSX provider. One public Common V2 export.
 * Native driver/relocator retained; only qualified CORE probing is exposed.
 */
#include <infiltratr/filesystem_module_v2.h>
#include <stdint.h>
static uint16_t be16(const unsigned char *p) {
    return (uint16_t)((uint16_t)p[0]<<8 | (uint16_t)p[1]);
}
static uint32_t be32(const unsigned char *p) {
    return (uint32_t)p[0]<<24 | (uint32_t)p[1]<<16 |
           (uint32_t)p[2]<<8 | (uint32_t)p[3];
}
static int probe(const IfsFsHostRuntimeV2 *host,uint32_t *variant)
{
    unsigned char header[512];
    uint16_t sig,version;
    uint32_t block_bytes,total_blocks,free_blocks;
    if (!variant || !ifs_fs_host_v2_valid(host)) return IFS_FS_INVALID;
    *variant=0u;
    if(host->volume_bytes<1536u || host->maximum_transfer_bytes<sizeof(header))
        return IFS_FS_NOT_FOUND;
    if(host->read_at(host->context,1024u,header,sizeof(header))!=0)
        return IFS_FS_IO_ERROR;
    sig=be16(header);
    version=be16(header+2u);
    if ((sig!=UINT16_C(0x482b) || version!=4u) &&
        (sig!=UINT16_C(0x4858) || version!=5u))
        return IFS_FS_NOT_FOUND;
    block_bytes=be32(header+40u);
    total_blocks=be32(header+44u);
    free_blocks=be32(header+48u);
    if(block_bytes<512u || block_bytes%512u ||
       (block_bytes & (block_bytes-1u)) ||
       !total_blocks || free_blocks>total_blocks ||
       (uint64_t)total_blocks*block_bytes>host->volume_bytes)
        return IFS_FS_CORRUPT;
    *variant=sig==UINT16_C(0x4858)?5u:4u;
    return IFS_FS_OK;
}
static const IfsFsCoreOpsV2 core={
    .header={IFS_FS_GROUP_ABI_V2,sizeof(IfsFsCoreOpsV2),
             IFS_FS_GROUP_CORE,0u,IFS_FS_CORE_PROBE},
    .probe=probe
};
static const IfsFsGroupHeaderV2 *query(
    uint32_t id,uint32_t minimum,uint32_t maximum)
{
    if(id!=IFS_FS_GROUP_CORE || minimum>IFS_FS_GROUP_ABI_V2 ||
       maximum<IFS_FS_GROUP_ABI_V2) return NULL;
    return &core.header;
}
__attribute__((used,section(".ifsmeta")))
static const char identity[]="IFS-MODULE:hfsplus:common2";
static const IfsFsModuleApiV2 descriptor={
    .abi_version=IFS_FS_COMMON_ABI_V2,
    .structure_bytes=sizeof(IfsFsModuleApiV2),
    .format_id="hfsplus",
    .runtime_profiles=IFS_FS_PROFILE_HOSTED,
    .group_mask=IFS_FS_GROUP_BIT(IFS_FS_GROUP_CORE),
    .query_group=query
};
__attribute__((visibility("default")))
const IfsFsModuleApiV2 *infiltrator_filesystem_api_v2(void) {
    return &descriptor;
}
