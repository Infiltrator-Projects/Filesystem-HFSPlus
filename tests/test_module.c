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
static int rd(void *ctx,uint64_t at,void *data,uint32_t n) {
    (void)ctx;
    if(fail_read || !data || at>MEDIA_BYTES || n>MEDIA_BYTES-at) return -1;
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
    CHECK(api->group_mask==IFS_FS_GROUP_BIT(IFS_FS_GROUP_CORE));
    const IfsFsCoreOpsV2 *core=
        (const IfsFsCoreOpsV2 *)ifs_fs_group_v2(api,IFS_FS_GROUP_CORE);
    CHECK(core && core->header.capabilities==IFS_FS_CORE_PROBE);
    CHECK(core->probe && !core->mount && !core->write);
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
    u16(1024u,0x4858u);u16(1026u,5u);
    CHECK(core->probe(&host,&variant)==IFS_FS_OK && variant==5u);
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
    puts("HFSPLUS-STRICT-V2-PROBE-PASS");
    return 0;
}
