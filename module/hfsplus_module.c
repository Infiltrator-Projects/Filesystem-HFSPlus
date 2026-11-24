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
#ifndef IFS_HFSPLUS_FIRMWARE_BUILD
static unsigned formatting;
#endif
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
#ifndef IFS_HFSPLUS_FIRMWARE_BUILD
    if(formatting){unlock_registry();return IFS_FS_BUSY;}
#endif
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
#ifndef IFS_HFSPLUS_FIRMWARE_BUILD
/* Empty, non-journalled HFS+/HFSX volumes, following Apple TN1150.
 * Restrict initial geometry and label encoding rather than advertise
 * unqualified Unicode normalization, journalling or arbitrary block sizes.
 * Every on-disk write is through the Common V2 host lease. */
enum { FORMAT_BLOCK=4096u, FORMAT_MIN_BLOCKS=8192u,
       FORMAT_MAX_BLOCKS=4194304u };
static void store16(uint8_t *p,uint32_t x)
{p[0]=(uint8_t)(x>>8);p[1]=(uint8_t)x;}
static void store32(uint8_t *p,uint32_t x)
{store16(p,x>>16);store16(p+2,x);}
static void store64(uint8_t *p,uint64_t x)
{store32(p,(uint32_t)(x>>32));store32(p+4,(uint32_t)x);}
static int format_write(const IfsFsHostRuntimeV2 *h,uint64_t at,const void *p,uint32_t n)
{
    if(h->cancelled && h->cancelled(h->context))return IFS_FS_BUSY;
    return ifs_fs_host_write_bytes_v2(h,at,p,n);
}
static void format_fork(uint8_t *p,uint32_t start,uint32_t blocks)
{
    store64(p,(uint64_t)blocks*FORMAT_BLOCK);
    store32(p+8,blocks*FORMAT_BLOCK);
    store32(p+12,blocks);store32(p+16,start);store32(p+20,blocks);
}
/* B-tree header node: 106-byte header, 128-byte user record, and node map.
 * Catalog uses variable-length index keys. Extents use fixed-length keys. */
static void format_tree(uint8_t *p,int catalog,uint32_t variant)
{
    memset(p,0,FORMAT_BLOCK);
    p[8]=1u;store16(p+10,3u);
    if(catalog) {
        store16(p+14,1u);store32(p+16,1u);store32(p+20,2u);
        store32(p+24,1u);store32(p+28,1u);
    }
    store16(p+32,FORMAT_BLOCK);store16(p+34,catalog?516u:10u);
    store32(p+36,catalog?2u:1u);
    store32(p+46,catalog?2u*FORMAT_BLOCK:FORMAT_BLOCK);
    p[51]=(uint8_t)(catalog?(variant==5u?0xbcu:0xcfu):0u);
    store32(p+52,catalog?6u:2u);
    p[248]=catalog?0xc0u:0x80u;
    store16(p+FORMAT_BLOCK-2u,14u);
    store16(p+FORMAT_BLOCK-4u,120u);
    store16(p+FORMAT_BLOCK-6u,248u);
    store16(p+FORMAT_BLOCK-8u,FORMAT_BLOCK-8u);
}
static uint32_t format_key(uint8_t *p,uint32_t parent,const char *name,uint32_t n)
{
    store16(p,6u+2u*n);store32(p+2,parent);store16(p+6,n);
    for(uint32_t i=0u;i<n;++i)store16(p+8u+2u*i,(unsigned char)name[i]);
    return 8u+2u*n;
}
static int format_impl(const IfsFsHostRuntimeV2 *h,uint32_t variant,
                       const char *label,uint32_t flags)
{
    const uint32_t blocks=(uint32_t)(h->volume_bytes/FORMAT_BLOCK);
    const uint32_t bitmap_blocks=(blocks+FORMAT_BLOCK*8u-1u)/(FORMAT_BLOCK*8u);
    const uint32_t extents=1u+bitmap_blocks,catalog=extents+1u;
    const uint32_t next=catalog+2u;
    const uint32_t used=next+1u;
    uint32_t n=(uint32_t)strlen(label),now=2082844800u;
    if(h->realtime_ns) {
        uint64_t t=h->realtime_ns(h->context)/1000000000ull;
        if(t<=UINT32_MAX-2082844800u)now+=(uint32_t)t;
    }
    (void)flags;
    uint8_t block[FORMAT_BLOCK]={0},vh[512]={0};
    int rc;
    /* Invalidate both volume headers and any boot blocks before touching
     * metadata; write the alternate then primary header only after flush. */
    rc=format_write(h,0u,block,sizeof(block));
    if(rc!=IFS_FS_OK)return rc;
    rc=format_write(h,h->volume_bytes-FORMAT_BLOCK,block,sizeof(block));
    if(rc!=IFS_FS_OK)return rc;
    if(h->flush(h->context)!=0)return IFS_FS_IO_ERROR;
    for(uint32_t i=0u;i<bitmap_blocks;++i) {
        memset(block,0,sizeof(block));
        for(uint32_t bit=0u;bit<FORMAT_BLOCK*8u;++bit) {
            uint32_t index=i*FORMAT_BLOCK*8u+bit;
            if(index<next || index>=blocks-1u)
                block[bit/8u]|=(uint8_t)(0x80u>>(bit%8u));
        }
        rc=format_write(h,(uint64_t)(i+1u)*FORMAT_BLOCK,block,sizeof(block));
        if(rc!=IFS_FS_OK)return rc;
    }
    format_tree(block,0,variant);
    rc=format_write(h,(uint64_t)extents*FORMAT_BLOCK,block,sizeof(block));
    if(rc!=IFS_FS_OK)return rc;
    format_tree(block,1,variant);
    rc=format_write(h,(uint64_t)catalog*FORMAT_BLOCK,block,sizeof(block));
    if(rc!=IFS_FS_OK)return rc;
    /* Leaf: root folder (parent CNID 1, volume name) followed by its
     * folder thread (CNID 2, empty key name). Both records are mandatory. */
    memset(block,0,sizeof(block));
    block[8]=0xffu;block[9]=1u;store16(block+10,2u);
    uint32_t at=14u;
    store16(block+FORMAT_BLOCK-2u,at);
    uint8_t *value=block+at+format_key(block+at,1u,label,n);
    /* Directory threads are mandatory; the file-only thread-exists flag
     * must remain clear on a folder record (TN1150). */
    store16(value,1u);store32(value+8,2u);
    store32(value+12,now);store32(value+16,now);
    store32(value+20,now);store32(value+24,now);
    store16(value+42,0040755u);
    at=(uint32_t)(value-block)+88u;
    store16(block+FORMAT_BLOCK-4u,at);
    value=block+at+format_key(block+at,2u,"",0u);
    store16(value,3u);store32(value+4,1u);store16(value+8,n);
    for(uint32_t i=0u;i<n;++i)store16(value+10u+2u*i,(unsigned char)label[i]);
    at=(uint32_t)(value-block)+10u+2u*n;
    store16(block+FORMAT_BLOCK-6u,at);
    rc=format_write(h,(uint64_t)(catalog+1u)*FORMAT_BLOCK,block,sizeof(block));
    if(rc!=IFS_FS_OK)return rc;
    if(h->flush(h->context)!=0)return IFS_FS_IO_ERROR;
    store16(vh,variant==5u?0x4858u:0x482bu);store16(vh+2,variant);
    store32(vh+4,0x100u); /* clean unmount, no journal */
    store32(vh+8,0x31302e30u); /* native HFS+ compatible mount version */
    store32(vh+16,now);store32(vh+20,now);store32(vh+28,now);
    /* Folder count excludes the root folder. */
    store32(vh+40,FORMAT_BLOCK);store32(vh+44,blocks);
    store32(vh+48,blocks-used);store32(vh+52,next);
    store32(vh+56,FORMAT_BLOCK);store32(vh+60,FORMAT_BLOCK);
    store32(vh+64,16u);store32(vh+68,1u);
    store64(vh+72,1u); /* MacRoman encoding bitmap */
    if(h->structure_bytes>=sizeof(*h) && h->random_bytes) {
        rc=ifs_fs_host_random_bytes_v2(h,vh+104,8u);
        if(rc!=IFS_FS_OK)return rc;
    }
    format_fork(vh+112,1u,bitmap_blocks);
    format_fork(vh+192,extents,1u);
    format_fork(vh+272,catalog,2u);
    rc=format_write(h,h->volume_bytes-1024u,vh,sizeof(vh));
    if(rc!=IFS_FS_OK)return rc;
    if(h->flush(h->context)!=0)return IFS_FS_IO_ERROR;
    rc=format_write(h,1024u,vh,sizeof(vh));
    if(rc!=IFS_FS_OK)return rc;
    if(h->flush(h->context)!=0) {
        memset(vh,0,sizeof(vh));
        (void)ifs_fs_host_write_bytes_v2(h,1024u,vh,sizeof(vh));
        (void)ifs_fs_host_write_bytes_v2(h,h->volume_bytes-1024u,vh,sizeof(vh));
        (void)h->flush(h->context);
        return IFS_FS_IO_ERROR;
    }
    uint32_t observed=0u;
    rc=probe(h,&observed);
    if(rc!=IFS_FS_OK || observed!=variant)
        return rc!=IFS_FS_OK?rc:IFS_FS_CORRUPT;
    /* Verify the allocation bitmap and catalogue through the production
     * reader, not through a separate fixture interpretation. */
    HfsPlusCatalogV2 check;
    rc=hfsplus_catalog_open_v2(h,&check);
    if(rc!=IFS_FS_OK)return rc;
    hfsplus_catalog_close_v2(&check);
    return IFS_FS_OK;
}
static int format_volume(const IfsFsHostRuntimeV2 *h,uint32_t variant,
                          const char *label,uint32_t flags)
{
    if(!ifs_fs_host_v2_valid(h))return IFS_FS_INVALID;
    if((h->capabilities&IFS_FS_HOST_READ_ONLY) ||
       !(h->capabilities&IFS_FS_HOST_EXCLUSIVE) ||
       !(h->capabilities&IFS_FS_HOST_DURABLE))return IFS_FS_ACCESS;
    if((variant!=4u && variant!=5u) || flags ||
       h->logical_sector_bytes>FORMAT_BLOCK || h->volume_bytes%FORMAT_BLOCK ||
       h->volume_bytes/FORMAT_BLOCK<FORMAT_MIN_BLOCKS ||
       h->volume_bytes/FORMAT_BLOCK>FORMAT_MAX_BLOCKS)return IFS_FS_UNSUPPORTED;
    if(!label || !*label)label="Untitled";
    size_t n=strlen(label);
    if(n>63u)return IFS_FS_INVALID;
    for(size_t i=0;i<n;++i)
        if((unsigned char)label[i]<32u || (unsigned char)label[i]>126u ||
           label[i]==':' || label[i]=='/')return IFS_FS_INVALID;
    lock_registry();
    if(formatting){unlock_registry();return IFS_FS_BUSY;}
    for(unsigned i=0u;i<HFSPLUS_MAX_SESSIONS;++i)
        if(sessions[i].token || sessions[i].busy) {
            unlock_registry();return IFS_FS_BUSY;
        }
    formatting=1u;unlock_registry();
    const int rc=format_impl(h,variant,label,flags);
    lock_registry();formatting=0u;unlock_registry();
    return rc;
}
#endif

static const IfsFsVolumeOpsV2 volume_ops={
    .header={IFS_FS_GROUP_ABI_V2,sizeof(IfsFsVolumeOpsV2),
             IFS_FS_GROUP_VOLUME,0u,IFS_FS_VOLUME_QUERY
#ifndef IFS_HFSPLUS_FIRMWARE_BUILD
             |IFS_FS_VOLUME_FORMAT
#endif
    },
    .query=query_volume
#ifndef IFS_HFSPLUS_FIRMWARE_BUILD
    ,.format=format_volume
#endif
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
