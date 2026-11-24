/* SPDX-License-Identifier: GPL-3.0-or-later
 * HFS+ and HFSX: strict Common V2 probe/read-only mount/volume query.
 * Uses validated on-media HFS+ volume-header geometry. The existing
 * native writer and recovery engines remain source-owned but are not yet
 * advertised through Common V2.
 */
#include <infiltratr/filesystem_module_v2.h>
#include "hfsplus_catalog_v2.h"
#include <stdint.h>
#include <stdatomic.h>
#include <stddef.h>
#include <string.h>
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
    if(!variant)return IFS_FS_INVALID;
    *variant=0u;
    if(!ifs_fs_host_v2_valid(host))return IFS_FS_INVALID;
    if(host->volume_bytes<1536u)return IFS_FS_NOT_FOUND;
    {
        int rc=ifs_fs_host_read_bytes_v2(host,1024u,header,
                                        (uint32_t)sizeof(header));
        if(rc!=IFS_FS_OK)return rc;
    }
    sig=be16(header);
    version=be16(header+2u);
    if(sig!=UINT16_C(0x482b) && sig!=UINT16_C(0x4858))
        return IFS_FS_NOT_FOUND;
    /* Recognised native volume, unsupported revision: never let a BOOT
     * format scan mistake this for an unrelated or absent filesystem. */
    if((sig==UINT16_C(0x482b) && version!=4u) ||
       (sig==UINT16_C(0x4858) && version!=5u))
        return IFS_FS_UNSUPPORTED;
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
static int probe(const IfsFsHostRuntimeV2 *host, uint32_t *variant)
{
    return examine(host, variant, NULL);
}
/* Each mount owns its independent immutable format state. Opaque tokens
 * never point at reusable storage and are never recycled. */
#define HFSPLUS_MAX_SESSIONS 32u
typedef struct {
    uintptr_t token;
    unsigned busy;
    IfsFsHostRuntimeV2 host;
    IfsFsVolumeInfoV2 info;
    HfsPlusCatalogV2 catalog;
} HfsPlusSession;
static HfsPlusSession sessions[HFSPLUS_MAX_SESSIONS];
static uintptr_t token_sequence;
/* A short registry lock protects token allocation and exclusive per-volume
 * operation leases. The lock is never held across host I/O. A busy volume
 * returns BUSY rather than racing catalogue state or blocking a callback. */
static atomic_flag registry_guard=ATOMIC_FLAG_INIT;
static void lock_registry(void)
{
    while (atomic_flag_test_and_set_explicit(&registry_guard,memory_order_acquire)) {}
}
static void unlock_registry(void)
{
    atomic_flag_clear_explicit(&registry_guard,memory_order_release);
}
static int acquire_mount(void *handle,HfsPlusSession **out)
{
    int rc=IFS_FS_INVALID;
    *out=NULL;
    if (!handle) return rc;
    lock_registry();
    for (unsigned i=0;i<HFSPLUS_MAX_SESSIONS;++i) {
        if (sessions[i].token!=(uintptr_t)handle) continue;
        if (sessions[i].busy) rc=IFS_FS_BUSY;
        else {
            sessions[i].busy=1u;
            *out=&sessions[i];
            rc=IFS_FS_OK;
        }
        break;
    }
    unlock_registry();
    return rc;
}
static int release_mount(HfsPlusSession *slot,int rc)
{
    lock_registry();
    slot->busy=0u;
    unlock_registry();
    return rc;
}
static int mount_volume(const IfsFsHostRuntimeV2 *host,void **volume,
                        IfsFsVolumeInfoV2 *info)
{
    IfsFsVolumeInfoV2 measured={0};
    HfsPlusSession *slot=NULL;
    uint32_t variant=0;
    if(!volume || !info || info->structure_bytes<sizeof(*info) ||
       !ifs_fs_host_v2_valid(host))return IFS_FS_INVALID;
    *volume=NULL;
    if(!(host->capabilities&IFS_FS_HOST_READ_ONLY))return IFS_FS_ACCESS;
    int rc=examine(host,&variant,&measured);
    if (rc!=IFS_FS_OK) return rc;
    lock_registry();
    for (unsigned i=0;i<HFSPLUS_MAX_SESSIONS;++i)
        if (!sessions[i].token && !sessions[i].busy) {
            slot=&sessions[i];
            break;
        }
    if (!slot || token_sequence==UINTPTR_MAX) {
        unlock_registry();
        return IFS_FS_BUSY;
    }
    /* Reserve the stable slot before opening the catalogue, but do not
     * publish its token until the native reader has fully initialized. */
    slot->busy=1u;
    const uintptr_t token=++token_sequence;
    unlock_registry();
    memset(&slot->host,0,sizeof(slot->host));
    size_t copied=host->structure_bytes<sizeof(slot->host)?
                  host->structure_bytes:sizeof(slot->host);
    memcpy(&slot->host,host,copied);
    slot->host.structure_bytes=(uint32_t)copied;
    slot->info=measured;
    rc=hfsplus_catalog_open_v2(&slot->host,&slot->catalog);
    if (rc!=IFS_FS_OK) {
        slot->host=(IfsFsHostRuntimeV2){0};
        slot->info=(IfsFsVolumeInfoV2){0};
        return release_mount(slot,rc);
    }
    lock_registry();
    slot->token=token;
    slot->busy=0u;
    *info=measured;
    *volume=(void *)token;
    unlock_registry();
    return IFS_FS_OK;
}
static int unmount_volume(void *volume)
{
    HfsPlusSession *slot;
    int rc=acquire_mount(volume,&slot);
    if (rc!=IFS_FS_OK) return rc;
    hfsplus_catalog_close_v2(&slot->catalog);
    slot->host=(IfsFsHostRuntimeV2){0};
    slot->info=(IfsFsVolumeInfoV2){0};
    lock_registry();
    slot->token=0u;
    slot->busy=0u;
    unlock_registry();
    return IFS_FS_OK;
}
static int query_volume(void *volume,IfsFsVolumeInfoV2 *info)
{
    HfsPlusSession *slot;
    if (!info || info->structure_bytes<sizeof(*info)) return IFS_FS_INVALID;
    int rc=acquire_mount(volume,&slot);
    if (rc!=IFS_FS_OK) return rc;
    *info=slot->info;
    return release_mount(slot,IFS_FS_OK);
}
static int stat_path(void *volume,const char *path,IfsFsEntryV2 *entry)
{
    HfsPlusSession *slot;
    int rc=acquire_mount(volume,&slot);
    if (rc!=IFS_FS_OK) return rc;
    return release_mount(slot,hfsplus_catalog_stat_v2(&slot->catalog,path,entry));
}
static int list_path(void *volume,const char *path,uint32_t index,
                     IfsFsEntryV2 *entry)
{
    HfsPlusSession *slot;
    int rc=acquire_mount(volume,&slot);
    if (rc!=IFS_FS_OK) return rc;
    return release_mount(slot,hfsplus_catalog_list_v2(&slot->catalog,path,index,entry));
}
static int read_path(void *volume,const char *path,uint64_t offset,
                     void *destination,uint32_t length)
{
    HfsPlusSession *slot;
    int rc=acquire_mount(volume,&slot);
    if (rc!=IFS_FS_OK) return rc;
    return release_mount(slot,hfsplus_catalog_read_v2(&slot->catalog,path,offset,destination,length));
}
static const IfsFsCoreOpsV2 core={
    .header={IFS_FS_GROUP_ABI_V2,sizeof(IfsFsCoreOpsV2),
             IFS_FS_GROUP_CORE,0u,IFS_FS_CORE_PROBE|IFS_FS_CORE_MOUNT|
             IFS_FS_CORE_STAT|IFS_FS_CORE_LIST|IFS_FS_CORE_READ},
    .probe=probe,.mount=mount_volume,.unmount=unmount_volume,
    .stat=stat_path,.directory_entry=list_path,.read=read_path
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
    #ifdef IFS_HFSPLUS_FIRMWARE_BUILD
    .runtime_profiles=IFS_FS_PROFILE_FREESTANDING|IFS_FS_PROFILE_FIRMWARE,
#else
    .runtime_profiles=IFS_FS_PROFILE_HOSTED,
#endif
    .group_mask=IFS_FS_GROUP_BIT(IFS_FS_GROUP_CORE)|
                IFS_FS_GROUP_BIT(IFS_FS_GROUP_VOLUME),
    .query_group=query
};
__attribute__((visibility("default")))
const IfsFsModuleApiV2 *infiltrator_filesystem_api_v2(void) {
    return &descriptor;
}
