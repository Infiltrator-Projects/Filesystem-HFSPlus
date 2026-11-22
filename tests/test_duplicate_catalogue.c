/* SPDX-License-Identifier: GPL-3.0-or-later
 * Source-owned Common V2 catalogue regression; no mock filesystem API.
 * The same hfsplus_catalog_v2.c is built into hosted and firmware HFSPLUS.so.
 */
#include "../native/hfsplus_catalog_v2.c"
#include <stdio.h>
#include <stdlib.h>

#define CHECK(x) do { if(!(x)) { \
  fprintf(stderr,"HFS+ duplicate catalogue line %u: %s\n", \
          (unsigned)__LINE__,#x); return 1; \
} } while(0)
typedef struct {
    uint8_t disk[4096];
    unsigned allocations,releases;
} Fixture;
static int read_sector(void *p,uint64_t offset,void *out,uint32_t n)
{
    Fixture *f=p;
    if(!n || offset%512u || n%512u || offset>sizeof(f->disk) ||
       n>sizeof(f->disk)-offset)return -1;
    memcpy(out,f->disk+(size_t)offset,n);
    return 0;
}
static void *allocate(void *p,size_t n)
{
    void *m=malloc(n);
    if(m)++((Fixture *)p)->allocations;
    return m;
}
static void release(void *p,void *m)
{
    if(m)++((Fixture *)p)->releases;
    free(m);
}
static void be16(uint8_t *p,uint16_t n)
{
    p[0]=(uint8_t)(n>>8u);p[1]=(uint8_t)n;
}
static void be32(uint8_t *p,uint32_t n)
{
    p[0]=(uint8_t)(n>>24u);p[1]=(uint8_t)(n>>16u);
    p[2]=(uint8_t)(n>>8u);p[3]=(uint8_t)n;
}
static void directory(uint8_t *p,uint32_t id)
{
    be16(p,8u);           /* 6-byte fixed key + 1 UTF-16BE name */
    be32(p+2u,2u);       /* parent CNID of root */
    be16(p+6u,1u);
    be16(p+8u,(uint16_t)'x');
    be16(p+10u,1u);     /* directory record */
    be32(p+18u,id);
}
int main(void)
{
    Fixture f={0};
    IfsFsHostRuntimeV2 host={
        .abi_version=IFS_FS_HOST_ABI_V2,
        .structure_bytes=sizeof(host),
        .capabilities=IFS_FS_HOST_READ_ONLY,
        .volume_bytes=sizeof(f.disk),
        .logical_sector_bytes=512u,
        .physical_sector_bytes=512u,
        .maximum_transfer_bytes=512u,
        .context=&f,
        .read_at=read_sector,.allocate=allocate,.release=release
    };
    HfsPlusCatalogV2 v={0};
    v.host=&host;v.block_bytes=512u;v.blocks=8u;
    v.node_bytes=512u;v.node_count=2u;v.leaf_head=1u;
    v.variant=4u;
    v.catalog.length=1024u;v.catalog.total_blocks=2u;
    v.catalog.extents[0].start=2u;v.catalog.extents[0].count=2u;
    uint8_t *node=f.disk+1536u;
    node[8]=0xffu;
    be16(node+10u,1u);
    be16(node+510u,14u);
    be16(node+508u,114u);
    directory(node+14u,20u);
    HfsPlusScan scan={.parent=2u,.name="x",.name_bytes=1u};
    CHECK(catalog_scan(&v,&scan)==IFS_FS_OK);
    CHECK(scan.found && scan.result.id==20u);
    CHECK(f.allocations==f.releases);

    /* A separately indexed duplicate in a valid leaf may not be returned
     * as the first entry. The second differing CNID proves the ambiguity. */
    be16(node+10u,2u);
    be16(node+506u,214u);
    directory(node+114u,21u);
    scan=(HfsPlusScan){.parent=2u,.name="x",.name_bytes=1u};
    CHECK(catalog_scan(&v,&scan)==IFS_FS_CORRUPT);
    CHECK(f.allocations==f.releases);
    scan=(HfsPlusScan){.parent=2u,.name="missing",.name_bytes=7u};
    CHECK(catalog_scan(&v,&scan)==IFS_FS_NOT_FOUND);
    CHECK(f.allocations==f.releases);
    return 0;
}
