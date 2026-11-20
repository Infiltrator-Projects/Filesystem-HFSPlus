/* SPDX-License-Identifier: GPL-3.0-or-later
 * Independent HFS-generated media through the production Common V2 ABI.
 * The image opens O_RDONLY; its SHA-256 is compared by the runner.
 */
#define _GNU_SOURCE
#include <infiltratr/filesystem_module_v2.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
typedef struct { int fd; uint64_t bytes; } Device;
extern const IfsFsModuleApiV2 *infiltrator_filesystem_api_v2(void);
#define VERIFY(e) do {if(!(e)){ \
 fprintf(stderr,"HFSPLUS real-media Common V2 failure line %d: %s\n",__LINE__,#e); \
 return 1;}}while(0)
static int read_at(void *ctx,uint64_t offset,void *dst,uint32_t n)
{
    Device *d=ctx;
    if (!n || (offset%512u)||(n%512u)||n>4096u ||
        offset>d->bytes || n>d->bytes-offset) return -1;
    return pread(d->fd,dst,n,(off_t)offset)==(ssize_t)n?0:-1;
}
static void *allocate(void *ctx,size_t count)
{(void)ctx;return malloc(count);}
static void release(void *ctx,void *data)
{(void)ctx;free(data);}
static int compare_file(const IfsFsCoreOpsV2 *core,void *volume,
                        const char *hfs_path,const char *source_path)
{
    int fd=open(source_path,O_RDONLY|O_CLOEXEC);
    VERIFY(fd>=0);
    struct stat st;
    VERIFY(fstat(fd,&st)==0 && st.st_size>0);
    char name[128]={0};
    IfsFsEntryV2 info={.structure_bytes=sizeof(info),.name=name,
                        .name_capacity=sizeof(name)};
    VERIFY(core->stat(volume,hfs_path,&info)==IFS_FS_OK);
    VERIFY(info.type==IFS_FS_ENTRY_FILE);
    VERIFY(info.size_bytes==(uint64_t)st.st_size);
    unsigned char actual[16384],expected[16384];
    uint64_t off=0;
    while(off<(uint64_t)st.st_size) {
        uint32_t count=(uint64_t)st.st_size-off<sizeof(actual)?
                       (uint32_t)((uint64_t)st.st_size-off):
                       (uint32_t)sizeof(actual);
        VERIFY(pread(fd,expected,count,(off_t)off)==(ssize_t)count);
        VERIFY(core->read(volume,hfs_path,off,actual,count)==IFS_FS_OK);
        VERIFY(!memcmp(expected,actual,count));
        off+=count;
    }
    VERIFY(core->read(volume,hfs_path,(uint64_t)st.st_size,actual,1u)
           ==IFS_FS_INVALID);
    VERIFY(close(fd)==0);
    return 0;
}

/* Test-only forensic output when native catalogue iteration refuses an
 * externally formatted volume. Read bytes directly from disposable media
 * for diagnosis; acceptance still uses the actual V2 interface. */
static uint16_t diagnostics_be16(const unsigned char *p)
{ return (uint16_t)(((uint16_t)p[0]<<8)|p[1]); }
static uint32_t diagnostics_be32(const unsigned char *p)
{
    return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|
           ((uint32_t)p[2]<<8)|p[3];
}
static void diagnostics_catalog(int fd)
{
    unsigned char vh[512],header[128],node[32768];
    if(pread(fd,vh,sizeof(vh),1024)!=(ssize_t)sizeof(vh))return;
    uint32_t block_bytes=diagnostics_be32(vh+40);
    uint32_t block=diagnostics_be32(vh+272+16);
    if(!block_bytes || !block)return;
    uint64_t start=(uint64_t)block*block_bytes;
    if(pread(fd,header,sizeof(header),(off_t)start)!=(ssize_t)sizeof(header))
        return;
    uint32_t leaf=diagnostics_be32(header+24);
    uint32_t node_bytes=diagnostics_be16(header+32);
    fprintf(stderr,"HFSPLUS diagnostic catalog block=%u sector-bytes=%u leaf=%u node-bytes=%u\\n",
            block,block_bytes,leaf,node_bytes);
    if(node_bytes<512 || node_bytes>sizeof(node))return;
    uint64_t absolute=start+(uint64_t)leaf*node_bytes;
    if(pread(fd,node,node_bytes,(off_t)absolute)!=(ssize_t)node_bytes)
        return;
    uint32_t records=diagnostics_be16(node+10);
    if(records>200u)return;
    for(uint32_t i=0;i<records;++i) {
        uint32_t off=diagnostics_be16(node+node_bytes-2u*(i+1u));
        if(off+10u>node_bytes)continue;
        uint32_t n=diagnostics_be16(node+off+6);
        if(n>255u||off+8u+2u*n+2u>node_bytes)continue;
        uint32_t parent=diagnostics_be32(node+off+4);
        uint32_t type=diagnostics_be16(node+off+2u+
                                       diagnostics_be16(node+off));
        fprintf(stderr,"HFSPLUS catalog record %u parent=%u type=%u names=%u name:",
                i,parent,type,n);
        for(uint32_t j=0;j<n && j<80u;++j)
            fprintf(stderr," %04x",diagnostics_be16(node+off+8u+2u*j));
        fputc('\\n',stderr);
    }
}

int main(int argc,char **argv)
{
    if(argc!=4 && argc!=5)return 2;
    Device device={0};
    device.fd=open(argv[1],O_RDONLY|O_CLOEXEC|O_NOFOLLOW);
    VERIFY(device.fd>=0);
    struct stat st;
    VERIFY(fstat(device.fd,&st)==0 && st.st_size>0);
    device.bytes=(uint64_t)st.st_size;
    IfsFsHostRuntimeV2 host={
       .abi_version=IFS_FS_HOST_ABI_V2,
       .structure_bytes=sizeof(IfsFsHostRuntimeV2),
       .capabilities=IFS_FS_HOST_READ_ONLY,
       .volume_bytes=device.bytes,.logical_sector_bytes=512u,
       .physical_sector_bytes=512u,.maximum_transfer_bytes=4096u,
       .context=&device,.read_at=read_at,
       .allocate=allocate,.release=release};
    const IfsFsModuleApiV2 *api=infiltrator_filesystem_api_v2();
    VERIFY(ifs_fs_api_v2_valid(api,"hfsplus"));
    const IfsFsCoreOpsV2 *core=(const void *)
        ifs_fs_group_v2(api,IFS_FS_GROUP_CORE);
    VERIFY(core && core->probe && core->mount && core->unmount &&
           core->stat && core->directory_entry && core->read);
    void *volume=NULL;
    IfsFsVolumeInfoV2 info={.structure_bytes=sizeof(info)};
    uint32_t variant=0;
    if(argc==5 && !strcmp(argv[4],"--reject")) {
        VERIFY(core->probe(&host,&variant)!=IFS_FS_OK ||
               core->mount(&host,&volume,&info)!=IFS_FS_OK);
        if(volume)VERIFY(core->unmount(volume)==IFS_FS_OK);
        VERIFY(close(device.fd)==0);
        return 0;
    }
    if(argc==5 && !strcmp(argv[4],"--corrupt-catalog")) {
        VERIFY(core->probe(&host,&variant)==IFS_FS_OK);
        VERIFY(core->mount(&host,&volume,&info)==IFS_FS_OK && volume);
        char filename[128]={0};
        IfsFsEntryV2 item={.structure_bytes=sizeof(item),.name=filename,
                            .name_capacity=sizeof(filename)};
        VERIFY(core->directory_entry(volume,"/",0u,&item)==IFS_FS_CORRUPT);
        VERIFY(core->unmount(volume)==IFS_FS_OK);
        VERIFY(close(device.fd)==0);
        return 0;
    }
    if(argc==5) return 2;
    VERIFY(core->probe(&host,&variant)==IFS_FS_OK);
    VERIFY(core->mount(&host,&volume,&info)==IFS_FS_OK && volume);
    char name[128]={0};
    IfsFsEntryV2 entry={.structure_bytes=sizeof(entry),.name=name,
                        .name_capacity=sizeof(name)};
    VERIFY(core->stat(volume,"/",&entry)==IFS_FS_OK &&
           entry.type==IFS_FS_ENTRY_DIRECTORY);
    int found_kernel=0,found_initrd=0,found_end=0,found_unicode=0;
    for(uint32_t i=0;i<1024u;++i) {
        int rc=core->directory_entry(volume,"/",i,&entry);
        if(rc==IFS_FS_END){found_end=1;break;}
        if(rc!=IFS_FS_OK) {
            fprintf(stderr,"HFSPLUS directory index=%u status=%d\n",i,rc);
            diagnostics_catalog(device.fd);
        }
        VERIFY(rc==IFS_FS_OK);
        if(!strcmp(name,"kernel.img"))found_kernel=1;
        if(!strcmp(name,"initramfs"))found_initrd=1;
        if(strstr(name,".txt")!=NULL) {
            for(size_t j=0;j<entry.name_bytes;++j)
                if((unsigned char)name[j]>=0x80u)found_unicode=1;
            VERIFY(entry.name_bytes==strlen(name));
        }
    }
    VERIFY(found_kernel && found_initrd && found_unicode && found_end);
    VERIFY(compare_file(core,volume,"/kernel.img",argv[2])==0);
    VERIFY(compare_file(core,volume,"/initramfs",argv[3])==0);
    VERIFY(core->stat(volume,"/not-present",&entry)==IFS_FS_NOT_FOUND);
    VERIFY(core->unmount(volume)==IFS_FS_OK);
    VERIFY(close(device.fd)==0);
    puts("HFSPLUS_INDEPENDENT_NATIVE_COMMON_V2_REAL_READS_PASS");
    return 0;
}
