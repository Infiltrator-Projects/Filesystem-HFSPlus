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
static void u64(unsigned offset,uint64_t n)
{
    u32(offset,(uint32_t)(n>>32));
    u32(offset+4u,(uint32_t)n);
}
static void mark_block(unsigned block)
{
    media[4096u+(block>>3u)]|=(unsigned char)(0x80u>>(block&7u));
}
/* Independent byte-level genuine HFS+ volume header, allocation fork and
 * catalogue B-tree: root file, nested folder, and nested file. The native
 * decoder, not a fixture callback, interprets all disk records. */
static void catalog_record(unsigned start,uint32_t parent,const char *name,
                           uint32_t cnid,int directory,unsigned file_block,
                           const char *contents)
{
    unsigned length=(unsigned)strlen(name);
    u16(start,(uint16_t)(6u+2u*length));
    u32(start+2u,parent);
    u16(start+6u,(uint16_t)length);
    for(unsigned i=0;i<length;++i)u16(start+8u+2u*i,(unsigned char)name[i]);
    unsigned value=start+8u+2u*length;
    u16(value,(uint16_t)(directory?1u:2u));
    u32(value+8u,cnid);
    if(!directory) {
        size_t bytes=strlen(contents);
        u64(value+88u,bytes);
        u32(value+88u+12u,1u);
        u32(value+88u+16u,file_block);
        u32(value+88u+20u,1u);
        memcpy(media+4096u*file_block,contents,bytes);
    }
}
static void make_catalogue(void)
{
    /* Header: real HFS+ allocation-file fork at +112 and catalog at +272. */
    u64(1024u+112u,19u);
    u32(1024u+124u,1u);
    u32(1024u+128u,1u);
    u32(1024u+132u,1u);
    u64(1024u+272u,2048u);
    u32(1024u+284u,1u);
    u32(1024u+288u,2u);
    u32(1024u+292u,1u);
    for(unsigned i=0u;i<80u;++i)mark_block(i);
    const unsigned tree=2u*4096u,leaf=tree+512u,next=tree+1024u;
    media[tree+8u]=1u;
    u16(tree+10u,1u);
    u16(tree+14u,1u); /* depth */
    u32(tree+16u,1u); /* root node */
    u32(tree+20u,4u); /* leaf records */
    u32(tree+24u,1u); /* first leaf */
    u32(tree+28u,3u); /* last leaf */
    u16(tree+32u,512u);
    u16(tree+34u,516u);
    u32(tree+36u,4u);
    media[tree+51u]=0xcfu;
    u32(leaf,2u);
    media[leaf+8u]=0xffu;media[leaf+9u]=1u;
    u16(leaf+10u,2u);
    catalog_record(leaf+14u,2u,"kernel.img",16u,0,3u,"HFSPLUS-KERNEL-V2");
    catalog_record(leaf+290u,2u,"Boot",17u,1,0u,"");
    u16(leaf+510u,14u);u16(leaf+508u,290u);u16(leaf+506u,394u);
    u32(next+4u,1u);
    media[next+8u]=0xffu;media[next+9u]=1u;
    u16(next+10u,1u);
    catalog_record(next+14u,17u,"initramfs",18u,0,4u,"HFSPLUS-INITRAMFS-V2");
    u16(next+510u,14u);u16(next+508u,288u);
    /* A third genuine B-tree leaf contains an HFS+ legal CR filename.
     * It must be accessible through the same name returned by LIST. */
    u32(next,3u);
    const unsigned last=tree+1536u;
    u32(last+4u,2u);
    media[last+8u]=0xffu;media[last+9u]=1u;
    u16(last+10u,1u);
    catalog_record(last+14u,17u,"line\rbreak",19u,0,5u,"HFSPLUS-CR-NAME-V2");
    u16(last+510u,14u);u16(last+508u,290u);
}

#define CHECK(x) do {if(!(x)){fprintf(stderr,"HFS+ V2 failed: %s at %d\n",#x,__LINE__);return 1;}}while(0)
static int forbidden_write(void *ctx,uint64_t at,const void *src,uint32_t n)
{ (void)ctx;(void)at;(void)src;(void)n;return -1; }
static int forbidden_flush(void *ctx)
{ (void)ctx;return -1; }
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
    CHECK(core && core->header.capabilities==
          (IFS_FS_CORE_PROBE|IFS_FS_CORE_MOUNT|
           IFS_FS_CORE_STAT|IFS_FS_CORE_LIST|IFS_FS_CORE_READ));
    CHECK(core->stat && core->directory_entry && core->read);
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
    make_catalogue();
    CHECK(core->probe(&host,&variant)==IFS_FS_OK && variant==4u);
    IfsFsVolumeInfoV2 info={0},snapshot={0};
    void *volume=NULL,*stale=NULL;
    info.structure_bytes=sizeof(info);
    host.capabilities=IFS_FS_HOST_EXCLUSIVE|IFS_FS_HOST_DURABLE;
    host.write_at=forbidden_write;host.flush=forbidden_flush;
    CHECK(ifs_fs_host_v2_valid(&host));
    CHECK(core->probe(&host,&variant)==IFS_FS_OK);
    CHECK(core->mount(&host,&volume,&info)==IFS_FS_ACCESS && !volume);
    host.capabilities=IFS_FS_HOST_READ_ONLY;
    host.write_at=NULL;host.flush=NULL;
    CHECK(ifs_fs_host_v2_valid(&host));
    CHECK(core->mount(&host,&volume,&info)==IFS_FS_OK && volume);
    stale=volume;
    CHECK(info.variant==4u && info.block_bytes==4096u &&
          info.total_bytes==614400u && info.free_bytes==286720u &&
          info.max_name_bytes==765u);
    {
        char name[128]={0},buffer[64]={0};
        IfsFsEntryV2 entry={.structure_bytes=sizeof(entry),
                            .name=name,.name_capacity=sizeof(name)};
        uint8_t original[MEDIA_BYTES];
        memcpy(original,media,sizeof(media));
        CHECK(core->stat(volume,"/",&entry)==IFS_FS_OK &&
              entry.type==IFS_FS_ENTRY_DIRECTORY);
        CHECK(core->stat(volume,"/kernel.img",&entry)==IFS_FS_OK &&
              entry.size_bytes==strlen("HFSPLUS-KERNEL-V2"));
        CHECK(core->directory_entry(volume,"/",0u,&entry)==IFS_FS_OK &&
              !strcmp(name,"kernel.img"));
        CHECK(core->directory_entry(volume,"/",1u,&entry)==IFS_FS_OK &&
              !strcmp(name,"Boot"));
        CHECK(core->directory_entry(volume,"/",2u,&entry)==IFS_FS_END);
        CHECK(core->stat(volume,"/Boot/initramfs",&entry)==IFS_FS_OK &&
              entry.type==IFS_FS_ENTRY_FILE);
        CHECK(core->read(volume,"/Boot/initramfs",0u,buffer,
                         (uint32_t)strlen("HFSPLUS-INITRAMFS-V2"))==IFS_FS_OK &&
              !memcmp(buffer,"HFSPLUS-INITRAMFS-V2",
                      strlen("HFSPLUS-INITRAMFS-V2")));
        CHECK(core->directory_entry(volume,"/Boot",0u,&entry)==IFS_FS_OK &&
              !strcmp(name,"initramfs"));
        CHECK(core->directory_entry(volume,"/Boot",1u,&entry)==IFS_FS_OK &&
              !strcmp(name,"line\rbreak"));
        CHECK(core->directory_entry(volume,"/Boot",2u,&entry)==IFS_FS_END);
        CHECK(core->stat(volume,"/Boot/line\rbreak",&entry)==IFS_FS_OK &&
              entry.size_bytes==strlen("HFSPLUS-CR-NAME-V2"));
        CHECK(core->read(volume,"/Boot/line\rbreak",0u,buffer,
                         (uint32_t)strlen("HFSPLUS-CR-NAME-V2"))==IFS_FS_OK &&
              !memcmp(buffer,"HFSPLUS-CR-NAME-V2",
                      strlen("HFSPLUS-CR-NAME-V2")));
        CHECK(core->stat(volume,"/Boot/line\tbreak",&entry)==IFS_FS_INVALID);
        CHECK(core->read(volume,"/kernel.img",8u,buffer,6u)==IFS_FS_OK &&
              !memcmp(buffer,"KERNEL",6u));
        CHECK(core->stat(volume,"/notfound",&entry)==IFS_FS_NOT_FOUND);
        CHECK(core->read(volume,"/Boot/../kernel.img",0u,buffer,1u)
              ==IFS_FS_INVALID);
        CHECK(!memcmp(original,media,sizeof(media)));
    }
    snapshot.structure_bytes=sizeof(snapshot);
    CHECK(ops->query(volume,&snapshot)==IFS_FS_OK &&
          snapshot.total_bytes==info.total_bytes);
    {
        void *parallel_volume=NULL;
        IfsFsVolumeInfoV2 parallel={.structure_bytes=sizeof(parallel)};
        CHECK(core->mount(&host,&parallel_volume,&parallel)==IFS_FS_OK &&
              parallel_volume && parallel_volume!=volume);
        CHECK(parallel.free_bytes==info.free_bytes);
        parallel.structure_bytes=sizeof(parallel);
        CHECK(ops->query(parallel_volume,&parallel)==IFS_FS_OK);
        CHECK(core->unmount(parallel_volume)==IFS_FS_OK);
        parallel.structure_bytes=sizeof(parallel);
        CHECK(ops->query(parallel_volume,&parallel)==IFS_FS_INVALID);
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
    CHECK(core->probe(&host,&variant)==IFS_FS_UNSUPPORTED);
    info.structure_bytes=sizeof(info);
    volume=NULL;
    CHECK(core->mount(&host,&volume,&info)==IFS_FS_UNSUPPORTED && !volume);
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
