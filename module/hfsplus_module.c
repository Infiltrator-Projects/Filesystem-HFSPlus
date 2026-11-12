/* SPDX-License-Identifier: GPL-3.0-or-later
 * HFS+ and HFSX: strict Common V2 probe/read-only mount/volume query.
 * Uses validated on-media HFS+ volume-header geometry. The existing
 * native writer and recovery engines remain source-owned but are not yet
 * advertised through Common V2.
 */
#include <infiltratr/filesystem_module_v2.h>
#include <stdint.h>
#include <stddef.h>
static uint16_t be16(const unsigned char *p) {
    return (uint16_t)(((uint16_t)p[0]<<8)|(uint16_t)p[1]);
}
static uint32_t be32(const unsigned char *p) {
    return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|
           ((uint32_t)p[2]<<8)|(uint32_t)p[3];
}
static int examine(const IfsFsHostRuntimeV2 *host,uint32_t *variant,
                   IfsFsVolumeInfoV2 *details)
{
    unsigned char header[512];
    uint16_t sig,version;
    uint32_t block_bytes,total_blocks,free_blocks;
    if(!variant || !ifs_fs_host_v2_valid(host))return IFS_FS_INVALID;
    *variant=0u;
    if(host->volume_bytes<1536u)return IFS_FS_NOT_FOUND;
    {
        int rc=ifs_fs_host_read_bytes_v2(host,1024u,header,
                                        (uint32_t)sizeof(header));
        if(rc!=IFS_FS_OK)return rc;
    }
    sig=be16(header);
    version=be16(header+2u);
    if((sig!=UINT16_C(0x482b) || version!=4u) &&
       (sig!=UINT16_C(0x4858) || version!=5u))
        return IFS_FS_NOT_FOUND;
    block_bytes=be32(header+40u);
    total_blocks=be32(header+44u);
    free_blocks=be32(header+48u);
    if(block_bytes<512u || block_bytes%512u ||
       (block_bytes&(block_bytes-1u)) ||
       !total_blocks || free_blocks>total_blocks ||
       (uint64_t)total_blocks*block_bytes>host->volume_bytes)
        return IFS_FS_CORRUPT;
    *variant=sig==UINT16_C(0x4858)?5u:4u;
    if(details) {
        details->structure_bytes=sizeof(*details);
        details->variant=*variant;
        details->max_name_bytes=765u; /* 255 HFS+ UTF-16 code units */
        details->block_bytes=block_bytes;
        details->total_bytes=(uint64_t)total_blocks*block_bytes;
        details->free_bytes=(uint64_t)free_blocks*block_bytes;
    }
    return IFS_FS_OK;
}
static int probe(const IfsFsHostRuntimeV2 *host,uint32_t *variant)
{
    return examine(host,variant,NULL);
}
/* A single image has one read-only mounted volume; generation tokens
 * prevent stale callers from reacquiring the next session. */
static struct {
    uintptr_t generation,active;
    IfsFsVolumeInfoV2 info;
} session;
static int live(void *volume)
{
    return session.active!=0u && (uintptr_t)volume==session.active;
}
static int mount_volume(const IfsFsHostRuntimeV2 *host,void **volume,
                        IfsFsVolumeInfoV2 *info)
{
    IfsFsVolumeInfoV2 measured={0};
    uint32_t variant=0u;
    int result;
    if(!volume || !info || info->structure_bytes<sizeof(*info))
        return IFS_FS_INVALID;
    *volume=NULL;
    if(!ifs_fs_host_v2_valid(host))return IFS_FS_INVALID;
    /* HFS+/HFSX CORE.MOUNT is not a write-capable transaction. Never
     * inherit permissions from an exclusive, writable host runtime. */
    if(!(host->capabilities & IFS_FS_HOST_READ_ONLY))return IFS_FS_ACCESS;
    if(session.active || session.generation==UINTPTR_MAX)return IFS_FS_BUSY;
    result=examine(host,&variant,&measured);
    if(result!=IFS_FS_OK)return result;
    session.active=++session.generation;
    session.info=measured;
    *info=measured;
    *volume=(void *)session.active;
    return IFS_FS_OK;
}
static int unmount_volume(void *volume)
{
    if(!live(volume))return IFS_FS_INVALID;
    session.active=0u;
    return IFS_FS_OK;
}
static int query_volume(void *volume,IfsFsVolumeInfoV2 *info)
{
    if(!live(volume) || !info || info->structure_bytes<sizeof(*info))
        return IFS_FS_INVALID;
    *info=session.info;
    return IFS_FS_OK;
}
static const IfsFsCoreOpsV2 core={
    .header={IFS_FS_GROUP_ABI_V2,sizeof(IfsFsCoreOpsV2),
             IFS_FS_GROUP_CORE,0u,IFS_FS_CORE_PROBE|IFS_FS_CORE_MOUNT},
    .probe=probe,.mount=mount_volume,.unmount=unmount_volume
};
static const IfsFsVolumeOpsV2 volume_ops={
    .header={IFS_FS_GROUP_ABI_V2,sizeof(IfsFsVolumeOpsV2),
             IFS_FS_GROUP_VOLUME,0u,IFS_FS_VOLUME_QUERY},
    .query=query_volume
};
static const IfsFsGroupHeaderV2 *query(
    uint32_t id,uint32_t minimum,uint32_t maximum)
{
    if(minimum>IFS_FS_GROUP_ABI_V2 || maximum<IFS_FS_GROUP_ABI_V2)return NULL;
    if(id==IFS_FS_GROUP_CORE)return &core.header;
    if(id==IFS_FS_GROUP_VOLUME)return &volume_ops.header;
    return NULL;
}
__attribute__((used,section(".ifsmeta")))
static const char identity[]="IFS-MODULE:hfsplus:common2";
static const IfsFsModuleApiV2 descriptor={
    .abi_version=IFS_FS_COMMON_ABI_V2,
    .structure_bytes=sizeof(IfsFsModuleApiV2),
    .format_id="hfsplus",
    .runtime_profiles=IFS_FS_PROFILE_HOSTED,
    .group_mask=IFS_FS_GROUP_BIT(IFS_FS_GROUP_CORE)|
                IFS_FS_GROUP_BIT(IFS_FS_GROUP_VOLUME),
    .query_group=query
};
__attribute__((visibility("default")))
const IfsFsModuleApiV2 *infiltrator_filesystem_api_v2(void) {
    return &descriptor;
}
