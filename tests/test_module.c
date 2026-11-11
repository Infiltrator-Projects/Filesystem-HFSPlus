/* SPDX-License-Identifier: GPL-3.0-or-later
 * Read-only Common V2 HFS+/HFSX probe, endian and hostile geometry checks.
 */
#include <infiltratr/filesystem_module_v2.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define MEDIA_BYTES (1024u*1024u)
static unsigned char media[MEDIA_BYTES];
static int fail_read;
static uint32_t logical_sector=512u,maximum_read=512u;
static int rd(void *ctx,uint64_t at,void *data,uint32_t n) {
    (void)ctx;
    if(fail_read || !data || (at%logical_sector) || (n%logical_sector) ||
       n>maximum_read || at>MEDIA_BYTES || n>MEDIA_BYTES-at)return -1;
    memcpy(data,media+(size_t)at,n);return 0;
}
static void *allocate(void *ctx,size_t n){(void)ctx;return malloc(n);}
static void release(void *ctx,void *p){(void)ctx;free(p);}
static void u16(unsigned n,uint16_t v) {
    media[n]=(unsigned char)(v>>8);media[n+1]=(unsigned char)v;
}
static void u32(unsigned n,uint32_t v) {
    media[n]=(unsigned char)(v>>24);media[n+1]=(unsigned char)(v>>16);
    media[n+2]=(unsigned char)(v>>8);media[n+3]=(unsigned char)v;
}
extern const IfsFsModuleApiV2 *infiltrator_filesystem_api_v2(void);
#define CHECK(x) do {if(!(x)){fprintf(stderr,"HFS+ V2 failed: %s at %d\n",#x,__LINE__);return 1;}}while(0)
int main(void) {
    const IfsFsModuleApiV2 *api=infiltrator_filesystem_api_v2();
    CHECK(ifs_fs_api_v2_valid(api,"hfsplus"));
    CHECK(api->runtime_profiles==IFS_FS_PROFILE_HOSTED);
    CHECK(api->group_mask==(IFS_FS_GROUP_BIT(IFS_FS_GROUP_CORE)|
                            IFS_FS_GROUP_BIT(IFS_FS_GROUP_VOLUME)));
    const IfsFsCoreOpsV2 *core=
        (const IfsFsCoreOpsV2 *)ifs_fs_group_v2(api,IFS_FS_GROUP_CORE);
    const IfsFsVolumeOpsV2 *ops=
        (const IfsFsVolumeOpsV2 *)ifs_fs_group_v2(api,IFS_FS_GROUP_VOLUME);
    CHECK(core && core->header.capabilities==(IFS_FS_CORE_PROBE|IFS_FS_CORE_MOUNT));
    CHECK(core->probe && core->mount && core->unmount && !core->write);
    CHECK(ops && ops->header.capabilities==IFS_FS_VOLUME_QUERY &&
          ops->query && !ops->format && !ops->repair);
    CHECK(!ifs_fs_group_v2(api,IFS_FS_GROUP_DEFRAG));
    IfsFsHostRuntimeV2 host={0};
    host.abi_version=IFS_FS_HOST_ABI_V2;
    host.structure_bytes=sizeof(host);
    host.capabilities=IFS_FS_HOST_READ_ONLY;
    host.volume_bytes=MEDIA_BYTES;
    host.logical_sector_bytes=512u;
    host.physical_sector_bytes=512u;
    host.maximum_transfer_bytes=512u;
    host.read_at=rd;
    host.allocate=allocate;
    host.release=release;
    CHECK(ifs_fs_host_v2_valid(&host));
    uint32_t variant=123u;
    CHECK(core->probe(&host,&variant)==IFS_FS_NOT_FOUND && variant==0u);
    u16(1024u,0x482bu);u16(1026u,4u);
    u32(1024u+40u,4096u);u32(1024u+44u,150u);u32(1024u+48u,70u);
    CHECK(core->probe(&host,&variant)==IFS_FS_OK && variant==4u);
    IfsFsVolumeInfoV2 info={0},snapshot={0};
    void *volume=NULL,*stale=NULL;
    info.structure_bytes=sizeof(info);
    CHECK(core->mount(&host,&volume,&info)==IFS_FS_OK && volume);
    stale=volume;
    CHECK(info.variant==4u && info.block_bytes==4096u &&
          info.total_bytes==614400u && info.free_bytes==286720u &&
          info.max_name_bytes==765u);
    snapshot.structure_bytes=sizeof(snapshot);
    CHECK(ops->query(volume,&snapshot)==IFS_FS_OK &&
          snapshot.total_bytes==info.total_bytes);
    {
        void *duplicate=NULL;
        CHECK(core->mount(&host,&duplicate,&snapshot)==IFS_FS_BUSY);
        CHECK(!duplicate);
    }
    CHECK(core->unmount(volume)==IFS_FS_OK);
    CHECK(core->unmount(stale)==IFS_FS_INVALID);
    CHECK(ops->query(stale,&snapshot)==IFS_FS_INVALID);
    u16(1024u,0x4858u);u16(1026u,5u);
    CHECK(core->probe(&host,&variant)==IFS_FS_OK && variant==5u);
    info.structure_bytes=sizeof(info);
    CHECK(core->mount(&host,&volume,&info)==IFS_FS_OK &&
          volume && volume!=stale && info.variant==5u);
    CHECK(ops->query(stale,&snapshot)==IFS_FS_INVALID);
    CHECK(core->unmount(volume)==IFS_FS_OK);
    /* Host I/O on 4K-logical-sector volumes must be sector aligned,
     * despite the HFS+ and HFSX volume header at byte 1024. */
    host.logical_sector_bytes=4096u;
    host.physical_sector_bytes=4096u;
    host.maximum_transfer_bytes=4096u;
    logical_sector=4096u;maximum_read=4096u;
    CHECK(ifs_fs_host_v2_valid(&host));
    CHECK(core->probe(&host,&variant)==IFS_FS_OK && variant==5u);
    info.structure_bytes=sizeof(info);
    CHECK(core->mount(&host,&volume,&info)==IFS_FS_OK &&
          info.variant==5u && info.free_bytes==286720u);
    CHECK(core->unmount(volume)==IFS_FS_OK);
    logical_sector=512u;maximum_read=512u;
    host.logical_sector_bytes=512u;
    host.physical_sector_bytes=512u;
    host.maximum_transfer_bytes=512u;
    u16(1026u,4u);
    CHECK(core->probe(&host,&variant)==IFS_FS_NOT_FOUND);
    u16(1026u,5u);
    u32(1024u+48u,151u);
    CHECK(core->probe(&host,&variant)==IFS_FS_CORRUPT);
    u32(1024u+48u,70u);u32(1024u+44u,UINT32_MAX);
    CHECK(core->probe(&host,&variant)==IFS_FS_CORRUPT);
    u32(1024u+44u,150u);
    fail_read=1;CHECK(core->probe(&host,&variant)==IFS_FS_IO_ERROR);
    fail_read=0;host.read_at=NULL;
    CHECK(core->probe(&host,&variant)==IFS_FS_INVALID);
    puts("HFSPLUS-STRICT-V2-MOUNT-QUERY-PASS");
    return 0;
}
