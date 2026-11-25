/* SPDX-License-Identifier: GPL-3.0-or-later
 * HFS+/HFSX Common V2 B-tree decoder, allocation file, data forks.
 * Native HFS+ structures and offsets follow Apple's on-media format.
 * No host filesystem paths, POSIX file descriptors or kernel VFS.
 */
#include "hfsplus_catalog_v2.h"
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#define HFSPLUS_ROOT_CNID 2u
#define HFSPLUS_FIRSTUSER_CNID 16u
#define HFSPLUS_MAX_NODE_BYTES 32768u
static uint16_t rd16(const uint8_t *p)
{ return (uint16_t)((uint16_t)p[0]<<8 | p[1]); }
static uint32_t rd32(const uint8_t *p)
{ return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3]; }
static uint64_t rd64(const uint8_t *p)
{ return ((uint64_t)rd32(p)<<32)|rd32(p+4); }

static int read_bytes(const IfsFsHostRuntimeV2 *h,uint64_t offset,
                      void *out,uint32_t n)
{
    if(!h || (!out && n) || offset>h->volume_bytes ||
       n>h->volume_bytes-offset)return IFS_FS_CORRUPT;
    if(!n)return IFS_FS_OK;
    return ifs_fs_host_read_bytes_v2(h,offset,out,n);
}
static void parse_fork(const uint8_t *p,HfsPlusForkV2 *f)
{
    f->length=rd64(p);
    f->total_blocks=rd32(p+12);
    for(uint32_t i=0;i<8u;++i) {
        f->extents[i].start=rd32(p+16+8*i);
        f->extents[i].count=rd32(p+20+8*i);
    }
}
static int fork_read_raw(const HfsPlusCatalogV2 *v,
                         const HfsPlusForkV2 *f,uint64_t off,void *out,uint32_t n)
{
    uint8_t *dst=out;
    if(!v || !f || (!out && n) || off>f->length ||
       n>f->length-off)return IFS_FS_INVALID;
    if(!n)return IFS_FS_OK;
    const uint64_t target=off+n;
    uint64_t logical=0;
    uint32_t accounted=0;
    for(uint32_t i=0;i<8u;++i) {
        const HfsPlusExtentV2 *e=&f->extents[i];
        if(e->start>v->blocks || e->count>v->blocks-e->start ||
           (e->count==0u && e->start!=0u))
            return IFS_FS_CORRUPT;
        if(e->count>UINT32_MAX-accounted)return IFS_FS_CORRUPT;
        accounted+=e->count;
        uint64_t span=(uint64_t)e->count*v->block_bytes;
        if(off>=logical && off<logical+span) {
            uint64_t chunk=logical+span-off;
            if(chunk>target-off)chunk=target-off;
            uint64_t physical=(uint64_t)e->start*v->block_bytes+(off-logical);
            int rc=read_bytes(v->host,physical,dst,(uint32_t)chunk);
            if(rc!=IFS_FS_OK)return rc;
            off+=chunk;dst+=chunk;
            if(off==target)return IFS_FS_OK;
        }
        logical+=span;
    }
    return IFS_FS_UNSUPPORTED; /* requires extents-overflow B-tree */
}
static int validate_extent_allocation(const HfsPlusCatalogV2 *v,
                                      const HfsPlusForkV2 *fork)
{
    if(!v->bitmap || !v->bitmap_bytes)return IFS_FS_INVALID;
    uint64_t span=0;
    uint32_t accounted=0;
    for(uint32_t i=0;i<8u;++i) {
        HfsPlusExtentV2 e=fork->extents[i];
        if(e.start>v->blocks || e.count>v->blocks-e.start ||
           (e.count==0u && e.start!=0u))
            return IFS_FS_CORRUPT;
        if(e.count>UINT32_MAX-accounted)return IFS_FS_CORRUPT;
        accounted+=e.count;
        span+=(uint64_t)e.count*v->block_bytes;
        for(uint32_t j=0;j<e.count;++j) {
            uint32_t block=e.start+j;
            if(block/8u>=v->bitmap_bytes ||
               !(v->bitmap[block>>3u]&(uint8_t)(0x80u>>(block&7u))))
                return IFS_FS_CORRUPT;
        }
    }
    if(fork->total_blocks<accounted)return IFS_FS_CORRUPT;
    if(fork->length>span || fork->total_blocks>accounted)
        return IFS_FS_UNSUPPORTED;
    return IFS_FS_OK;
}
void hfsplus_catalog_close_v2(HfsPlusCatalogV2 *v)
{
    if(!v)return;
    if(v->bitmap && v->host)
        v->host->release(v->host->context,v->bitmap);
    memset(v,0,sizeof(*v));
}
int hfsplus_catalog_open_v2(const IfsFsHostRuntimeV2 *host,HfsPlusCatalogV2 *v)
{
    uint8_t vh[512],head[64];
    if(!host || !v || !ifs_fs_host_v2_valid(host))
        return IFS_FS_INVALID;
    memset(v,0,sizeof(*v));
    int rc=read_bytes(host,1024u,vh,sizeof(vh));
    if(rc!=IFS_FS_OK)return rc;
    uint16_t sig=rd16(vh),version=rd16(vh+2);
    if(sig!=0x482bu && sig!=0x4858u) return IFS_FS_NOT_FOUND;
    if((sig==0x482bu && version!=4u) ||
       (sig==0x4858u && version!=5u)) return IFS_FS_UNSUPPORTED;
    v->host=host;
    v->variant=sig==0x4858u?5u:4u;
    v->block_bytes=rd32(vh+40);
    v->blocks=rd32(vh+44);
    uint32_t free_blocks=rd32(vh+48),attrib=rd32(vh+4);
    if(v->block_bytes<512u || v->block_bytes>65536u ||
       (v->block_bytes&(v->block_bytes-1u)) ||
       !v->blocks || free_blocks>v->blocks ||
       (uint64_t)v->blocks*v->block_bytes>host->volume_bytes)
        return IFS_FS_CORRUPT;
    /* A dirty journalled volume needs replay. Never read an uncommitted
     * catalogue snapshot as though it were clean. */
    if(attrib&(1u<<11))return IFS_FS_CORRUPT; /* incomplete native mutation */
    if((attrib&(1u<<13)) && !(attrib&(1u<<8)))
        return IFS_FS_UNSUPPORTED;
    parse_fork(vh+112,&v->bitmap_fork);
    parse_fork(vh+272,&v->catalog);
    if(!v->bitmap_fork.length || !v->catalog.length)
        return IFS_FS_UNSUPPORTED;
    v->bitmap_bytes=v->blocks/8u+(v->blocks%8u!=0u);
    if(v->bitmap_fork.length<v->bitmap_bytes)
        return IFS_FS_CORRUPT;
    v->bitmap=host->allocate(host->context,v->bitmap_bytes);
    if(!v->bitmap)return IFS_FS_NO_MEMORY;
    rc=fork_read_raw(v,&v->bitmap_fork,0,v->bitmap,v->bitmap_bytes);
    if(rc!=IFS_FS_OK)goto fail;
    rc=validate_extent_allocation(v,&v->bitmap_fork);
    if(rc!=IFS_FS_OK)goto fail;
    rc=validate_extent_allocation(v,&v->catalog);
    if(rc!=IFS_FS_OK)goto fail;
    uint64_t counted_free=0;
    for(uint32_t i=0;i<v->blocks;++i)
        if(!(v->bitmap[i>>3u]&(uint8_t)(0x80u>>(i&7u))))++counted_free;
    if(counted_free!=free_blocks){rc=IFS_FS_CORRUPT;goto fail;}
    rc=fork_read_raw(v,&v->catalog,0,head,sizeof(head));
    if(rc!=IFS_FS_OK)goto fail;
    if(head[8]!=1u || rd16(head+10u)<1u) {
        rc=IFS_FS_CORRUPT;goto fail;
    }
    v->node_bytes=rd16(head+32);
    v->node_count=rd32(head+36);
    v->leaf_head=rd32(head+24);
    if(v->node_bytes<512u || v->node_bytes>HFSPLUS_MAX_NODE_BYTES ||
       (v->node_bytes&(v->node_bytes-1u)) || !v->node_count ||
       (uint64_t)v->node_count*v->node_bytes>v->catalog.length ||
       (v->leaf_head && v->leaf_head>=v->node_count)) {
        rc=IFS_FS_CORRUPT;goto fail;
    }
    if(v->variant==5u && head[51]!=0xbcu && head[51]!=0xcfu) {
        rc=IFS_FS_UNSUPPORTED;goto fail;
    }
    v->case_sensitive=v->variant==5u && head[51]==0xbcu;
    return IFS_FS_OK;
fail:
    hfsplus_catalog_close_v2(v);
    return rc;
}
typedef struct {
    uint32_t parent,id,owner,group;
    uint16_t mode,utf16_length;
    uint8_t utf16[510];
    uint32_t type;
    HfsPlusForkV2 fork;
} HfsPlusEntry;
static int decode_record(const uint8_t *node,uint32_t node_bytes,
                         uint32_t begin,uint32_t finish,HfsPlusEntry *e)
{
    if(begin<14u || finish>node_bytes || finish<=begin ||
       finish-begin<10u)return IFS_FS_CORRUPT;
    const uint8_t *key=node+begin;
    uint32_t key_len=rd16(key);
    if(key_len<6u || key_len>516u || key_len+2u>finish-begin)
        return IFS_FS_CORRUPT;
    uint16_t name_len=rd16(key+6);
    if(name_len>255u || key_len!=6u+2u*name_len)
        return IFS_FS_CORRUPT;
    uint32_t data_offset=begin+2u+key_len;
    if(data_offset+2u>finish)return IFS_FS_CORRUPT;
    const uint8_t *data=node+data_offset;
    uint32_t left=finish-data_offset;
    memset(e,0,sizeof(*e));
    e->parent=rd32(key+2);
    e->utf16_length=name_len;
    memcpy(e->utf16,key+8,2u*name_len);
    uint16_t type=rd16(data);
    if(type==1u) {
        if(left<88u)return IFS_FS_CORRUPT;
        e->type=IFS_FS_ENTRY_DIRECTORY;
        e->id=rd32(data+8);
        if(e->id<2u)return IFS_FS_CORRUPT;
    } else if(type==2u) {
        if(left<248u)return IFS_FS_CORRUPT;
        e->type=IFS_FS_ENTRY_FILE;
        e->id=rd32(data+8);
        if(e->id<HFSPLUS_FIRSTUSER_CNID)return IFS_FS_CORRUPT;
        /* HFS+ compressed files store their actual contents in decmpfs
         * attributes; hardlinks redirect to the private inode directory.
         * Neither can safely be treated as an ordinary data fork without
         * the matching native attribute/hardlink engine. */
        if((rd16(data+2u)&0x000cu) != 0u ||
           (rd32(data+48u)==0x686c6e6bu &&
            rd32(data+52u)==0x6866732bu))
            return IFS_FS_UNSUPPORTED;
        parse_fork(data+88,&e->fork);
    } else if(type==3u || type==4u) {
        e->type=0u; /* catalog thread, never returned as child */
    } else return IFS_FS_CORRUPT;
    if(e->type) {
        /* HFSPlusBSDInfo at offset 32 in both file and folder records. */
        e->owner=rd32(data+32u);e->group=rd32(data+36u);
        e->mode=rd16(data+42u);
    }
    return IFS_FS_OK;
}
/* Native HFS+ catalogue keys are UTF-16BE, not ASCII. Convert all valid
 * code points, including supplementary-plane surrogate pairs, to UTF-8
 * without a locale or host library. HFS+ allows CR in a filename: do not
 * mistake a genuine on-disk entry for an unsupported filesystem feature.
 * Name comparison remains separately limited until Unicode collation is
 * fully qualified; this only implements truthful directory enumeration. */
static int simple_name(const HfsPlusEntry *e,char *dst,uint32_t capacity,
                       uint32_t *name_bytes)
{
    if(!e || !dst || !name_bytes || !capacity)return IFS_FS_INVALID;
    /* First validate the entire UTF-16BE sequence and its UTF-8 size.
     * Neither a trailing malformed surrogate nor insufficient capacity
     * may leave a partially written Common V2 directory name. */
    uint32_t required=0u;
    for(uint32_t i=0u;i<e->utf16_length;++i) {
        uint32_t cp=rd16(e->utf16+2u*i);
        if(cp>=0xd800u && cp<=0xdbffu) {
            if(++i>=e->utf16_length)return IFS_FS_CORRUPT;
            const uint32_t low=rd16(e->utf16+2u*i);
            if(low<0xdc00u || low>0xdfffu)return IFS_FS_CORRUPT;
            cp=0x10000u+((cp-0xd800u)<<10)+(low-0xdc00u);
        } else if(cp>=0xdc00u && cp<=0xdfffu) {
            return IFS_FS_CORRUPT;
        }
        if(!cp)return IFS_FS_CORRUPT;
        required+=cp<0x80u?1u:cp<0x800u?2u:cp<0x10000u?3u:4u;
    }
    if(required>=capacity)return IFS_FS_NO_SPACE;
    uint32_t used=0u;
    for(uint32_t i=0u;i<e->utf16_length;++i) {
        uint32_t cp=rd16(e->utf16+2u*i);
        if(cp>=0xd800u && cp<=0xdbffu) {
            if(++i>=e->utf16_length)return IFS_FS_CORRUPT;
            uint32_t low=rd16(e->utf16+2u*i);
            if(low<0xdc00u || low>0xdfffu)return IFS_FS_CORRUPT;
            cp=0x10000u+((cp-0xd800u)<<10)+(low-0xdc00u);
        } else if(cp>=0xdc00u && cp<=0xdfffu) {
            return IFS_FS_CORRUPT;
        }
        if(!cp)return IFS_FS_CORRUPT;
        uint32_t length=cp<0x80u?1u:cp<0x800u?2u:cp<0x10000u?3u:4u;
        if(length>=capacity-used)return IFS_FS_NO_SPACE;
        if(length==1u)dst[used++]=(char)cp;
        else if(length==2u) {
            dst[used++]=(char)(0xc0u|(cp>>6));
            dst[used++]=(char)(0x80u|(cp&0x3fu));
        } else if(length==3u) {
            dst[used++]=(char)(0xe0u|(cp>>12));
            dst[used++]=(char)(0x80u|((cp>>6)&0x3fu));
            dst[used++]=(char)(0x80u|(cp&0x3fu));
        } else {
            dst[used++]=(char)(0xf0u|(cp>>18));
            dst[used++]=(char)(0x80u|((cp>>12)&0x3fu));
            dst[used++]=(char)(0x80u|((cp>>6)&0x3fu));
            dst[used++]=(char)(0x80u|(cp&0x3fu));
        }
    }
    dst[used]=0;
    *name_bytes=used;
    return IFS_FS_OK;
}
/* Apple's HFS+ hard-link metadata lives in a deliberately hidden root
 * directory named four U+0000 code units plus "HFS+ Private Data".
 * Those NUL units are a documented on-disk sentinel, not a normal POSIX
 * pathname. Ignore only this exact private catalogue entry during public
 * namespace enumeration; do not mask arbitrary embedded NUL corruption.
 * Source: HFS Plus Volume Format TN1150, hard-link metadata directory. */
static int is_private_metadata_directory(const HfsPlusEntry *e)
{
    static const char suffix[]="HFS+ Private Data";
    const uint32_t suffix_length=(uint32_t)(sizeof(suffix)-1u);
    if(e->parent!=HFSPLUS_ROOT_CNID ||
       e->type!=IFS_FS_ENTRY_DIRECTORY ||
       e->utf16_length!=4u+suffix_length)return 0;
    for(uint32_t i=0u;i<4u;++i)
        if(rd16(e->utf16+2u*i)!=0u)return 0;
    for(uint32_t i=0u;i<suffix_length;++i)
        if(rd16(e->utf16+2u*(i+4u))!=(uint8_t)suffix[i])
            return 0;
    return 1;
}
typedef struct {
    uint32_t parent,wanted,count;
    const char *name;
    uint32_t name_bytes;
    HfsPlusEntry result;
    int found,unsupported,root_record,verify_allocation;
} HfsPlusScan;
static int matches(const HfsPlusCatalogV2 *v,const HfsPlusEntry *e,
                   const HfsPlusScan *s)
{
    if(s->root_record)return e->type==IFS_FS_ENTRY_DIRECTORY &&
        e->parent==1u && e->id==HFSPLUS_ROOT_CNID;
    if(!e->type || e->parent!=s->parent)return 0;
    if(!s->name)return 1;
    /* Compare the canonical UTF-8 bytes emitted by LIST. This guarantees
     * that a valid non-ASCII name returned by V2 LIST can be used by V2
     * STAT/READ, without falsely claiming unqualified Unicode collation,
     * normalization aliases or non-ASCII case-fold equivalences. */
    char utf8[4u*255u+1u];
    uint32_t bytes=0u;
    int rc=simple_name(e,utf8,sizeof(utf8),&bytes);
    if(rc==IFS_FS_CORRUPT)return -2;
    if(rc!=IFS_FS_OK)return -1;
    if(bytes!=s->name_bytes)return 0;
    for(uint32_t i=0u;i<bytes;++i) {
        uint8_t a=(uint8_t)utf8[i];
        uint8_t b=(uint8_t)s->name[i];
        if(!v->case_sensitive) {
            if(a>='A' && a<='Z')a=(uint8_t)(a+32u);
            if(b>='A' && b<='Z')b=(uint8_t)(b+32u);
        }
        if(a!=b)return 0;
    }
    return 1;
}
static int catalog_scan(const HfsPlusCatalogV2 *v,HfsPlusScan *scan)
{
    if(!v || !scan || !v->host)return IFS_FS_INVALID;
    if(!v->leaf_head)return IFS_FS_NOT_FOUND;
    uint8_t *node=v->host->allocate(v->host->context,v->node_bytes);
    if(!node)return IFS_FS_NO_MEMORY;
    uint32_t current=v->leaf_head,previous=0;
    int rc=IFS_FS_NOT_FOUND;
    for(uint32_t visited=0;visited<v->node_count;++visited) {
        rc=fork_read_raw(v,&v->catalog,(uint64_t)current*v->node_bytes,
                         node,v->node_bytes);
        if(rc!=IFS_FS_OK)break;
        uint32_t count=rd16(node+10);
        if(node[8]!=0xffu || rd32(node+4)!=previous ||
           (count+1u)*2u>v->node_bytes-14u){
            rc=IFS_FS_CORRUPT;break;
        }
        for(uint32_t index=0;index<count;++index) {
            uint32_t begin=rd16(node+v->node_bytes-2u*(index+1u));
            uint32_t finish=rd16(node+v->node_bytes-2u*(index+2u));
            if(finish>v->node_bytes-2u*(count+1u) || begin>=finish) {
                rc=IFS_FS_CORRUPT;goto done;
            }
            HfsPlusEntry record;
            rc=decode_record(node,v->node_bytes,begin,finish,&record);
            if(rc!=IFS_FS_OK)goto done;
            if(scan->verify_allocation) {
                if(record.type==IFS_FS_ENTRY_FILE) {
                    rc=validate_extent_allocation(v,&record.fork);
                    if(rc!=IFS_FS_OK)goto done;
                    HfsPlusForkV2 resource;
                    parse_fork(node+begin+2u+rd16(node+begin)+168u,&resource);
                    rc=validate_extent_allocation(v,&resource);
                    if(rc!=IFS_FS_OK)goto done;
                }
                continue;
            }
            if(is_private_metadata_directory(&record))
                continue;
            int eq=matches(v,&record,scan);
            if(eq<0){rc=eq==-2?IFS_FS_CORRUPT:IFS_FS_UNSUPPORTED;goto done;}
            if(eq>0) {
                if(scan->name || scan->root_record) {
                    /* The on-disk parent/name catalogue key is unique.
                     * Check subsequent leaves as well as the current leaf;
                     * accepting the first duplicate conceals corruption. */
                    if(scan->found){rc=IFS_FS_CORRUPT;goto done;}
                    scan->result=record;scan->found=1;
                } else if(scan->count++==scan->wanted) {
                    scan->result=record;scan->found=1;rc=IFS_FS_OK;goto done;
                }
            }
        }
        uint32_t next=rd32(node);
        if(!next){
            rc=scan->verify_allocation ||
                ((scan->name || scan->root_record) && scan->found)?IFS_FS_OK:IFS_FS_NOT_FOUND;
            goto done;
        }
        if(next>=v->node_count || next==current) {
            rc=IFS_FS_CORRUPT;goto done;
        }
        previous=current;current=next;
    }
    if(rc==IFS_FS_OK)rc=IFS_FS_CORRUPT;
done:
    v->host->release(v->host->context,node);
    return rc;
}
/* Publish only the native allocation file of a clean snapshot. Verify
 * the volume's special forks, both reserved header regions, and every
 * catalogued data/resource fork. Overflow extents and live journal
 * transactions require the missing native replay/overflow engine.
 */
static int validate_allocation_snapshot(HfsPlusCatalogV2 *v)
{
    uint8_t header[512];
    int rc=read_bytes(v->host,1024u,header,sizeof(header));
    if(rc!=IFS_FS_OK)return rc;
    const uint32_t attrs=rd32(header+4u);
    if(!(attrs&(1u<<8)) || (attrs&(1u<<11)))return IFS_FS_CORRUPT;
    if(attrs&(1u<<13))return IFS_FS_UNSUPPORTED;
    if(rd32(header+40u)!=v->block_bytes || rd32(header+44u)!=v->blocks)
        return IFS_FS_CORRUPT;
    for(unsigned offset=112u;offset<=432u;offset+=80u) {
        HfsPlusForkV2 fork;
        parse_fork(header+offset,&fork);
        rc=validate_extent_allocation(v,&fork);
        if(rc!=IFS_FS_OK)return rc;
    }
    const uint64_t fs_bytes=(uint64_t)v->blocks*v->block_bytes;
    if(fs_bytes<2048u)return IFS_FS_CORRUPT;
    for(uint32_t b=0u;b<1536u/v->block_bytes+(1536u%v->block_bytes!=0u);++b)
        if(!(v->bitmap[b>>3u]&(uint8_t)(0x80u>>(b&7u))))return IFS_FS_CORRUPT;
    for(uint64_t b=(fs_bytes-1024u)/v->block_bytes;b<v->blocks;++b)
        if(!(v->bitmap[b>>3u]&(uint8_t)(0x80u>>(b&7u))))return IFS_FS_CORRUPT;
    HfsPlusScan scan={.verify_allocation=1};
    rc=catalog_scan(v,&scan);
    if(rc==IFS_FS_NOT_FOUND)return IFS_FS_CORRUPT;
    return rc;
}
int hfsplus_allocation_map_v2(HfsPlusCatalogV2 *v,uint64_t first,
                             IfsFsAllocationRangeV2 *out,uint32_t capacity,
                             uint32_t *written)
{
    if(!written)return IFS_FS_INVALID;
    *written=0u;
    if(!v||!v->host||!v->bitmap||!out||!capacity||capacity>64u||
       first>v->host->volume_bytes)return IFS_FS_INVALID;
    if(v->mutation_fault)return IFS_FS_IO_ERROR;
    if(!first||!v->allocation_verified) {
        int rc=validate_allocation_snapshot(v);
        if(rc!=IFS_FS_OK)return rc;
        v->allocation_verified=1u;
    }
    const uint64_t limit=v->host->volume_bytes;
    if(first==limit)return IFS_FS_END;
    IfsFsAllocationRangeV2 page[64]={{0}};
    uint64_t at=first;
    uint32_t count=0u,scanned=0u;
    while(at<limit && scanned<65536u) {
        if(v->host->cancelled && v->host->cancelled(v->host->context))
            return IFS_FS_BUSY;
        uint64_t block=at/v->block_bytes,end;
        uint32_t state;
        if(block>=v->blocks) {
            state=IFS_FS_SPACE_RESERVED;end=limit;
        } else {
            state=(v->bitmap[block>>3u]&(uint8_t)(0x80u>>(block&7u)))?
                IFS_FS_SPACE_ALLOCATED:IFS_FS_SPACE_FREE;
            end=(block+1u)*v->block_bytes;
        }
        if(count && page[count-1u].state==state)page[count-1u].length+=end-at;
        else {
            if(count==capacity)break;
            page[count++]=(IfsFsAllocationRangeV2){
                .physical_offset=at,.length=end-at,.state=state};
        }
        at=end;++scanned;
    }
    memcpy(out,page,count*sizeof(*page));*written=count;
    return IFS_FS_OK;
}
static int lookup(const HfsPlusCatalogV2 *v,const char *path,HfsPlusEntry *e)
{
    if(!v || !path || *path!='/')return IFS_FS_INVALID;
    if(v->mutation_fault)return IFS_FS_IO_ERROR;
    memset(e,0,sizeof(*e));
    e->type=IFS_FS_ENTRY_DIRECTORY;e->id=HFSPLUS_ROOT_CNID;
    if(!path[1])return IFS_FS_OK;
    const char *part=path+1;
    uint32_t parent=HFSPLUS_ROOT_CNID;
    for(uint32_t depth=0;depth<64u;++depth) {
        const char *end=part;
        while(*end && *end!='/')++end;
        size_t length=(size_t)(end-part);
        if(length==0 || length>4u*255u || (length==1u && part[0]=='.') ||
           (length==2u && part[0]=='.' && part[1]=='.'))
            return IFS_FS_INVALID;
        /* HFS+ permits U+000D in on-disk names. LIST emits it, so
         * STAT and READ must accept that same canonical UTF-8 path.
         * Other ASCII control codes remain invalid. */
        for(size_t i=0;i<length;++i)
            if((uint8_t)part[i]<32u && part[i]!='\r')
                return IFS_FS_INVALID;
        HfsPlusScan s={.parent=parent,.name=part,.name_bytes=(uint32_t)length};
        int rc=catalog_scan(v,&s);
        if(rc!=IFS_FS_OK)return rc;
        *e=s.result;
        if(!*end)return IFS_FS_OK;
        if(e->type!=IFS_FS_ENTRY_DIRECTORY)return IFS_FS_NOT_FOUND;
        parent=e->id;part=end+1;
    }
    return IFS_FS_INVALID;
}
static int emit(const HfsPlusEntry *e,IfsFsEntryV2 *dst)
{
    if(!dst || dst->structure_bytes<sizeof(*dst) ||
       !dst->name)return IFS_FS_INVALID;
    uint32_t written=0u;
    int rc=simple_name(e,dst->name,dst->name_capacity,&written);
    if(rc!=IFS_FS_OK)return rc;
    dst->type=e->type;
    dst->name_bytes=written;
    dst->size_bytes=e->type==IFS_FS_ENTRY_FILE?e->fork.length:0u;
    dst->attributes=0;
    return IFS_FS_OK;
}
/* HFS+ stores the volume name in the root folder's catalog key,
 * not the volume header. Resolve the real CNID 2 record uniquely. */
static int root_record(const HfsPlusCatalogV2 *v,HfsPlusEntry *record)
{
    if(!v || v->mutation_fault)return IFS_FS_IO_ERROR;
    HfsPlusScan scan={.root_record=1};
    int rc=catalog_scan(v,&scan);
    if(rc==IFS_FS_OK)*record=scan.result;
    return rc;
}
int hfsplus_catalog_label_v2(const HfsPlusCatalogV2 *v,char *out,uint32_t cap,uint32_t *bytes)
{
    if(!bytes)return IFS_FS_INVALID;
    *bytes=0u;
    if(!out || !cap)return IFS_FS_INVALID;
    HfsPlusEntry record;
    int rc=root_record(v,&record);
    return rc==IFS_FS_OK?simple_name(&record,out,cap,bytes):rc;
}
int hfsplus_catalog_metadata_v2(const HfsPlusCatalogV2 *v,const char *path,IfsFsMetadataV2 *out)
{
    if(!path || !out || out->structure_bytes<sizeof(*out))return IFS_FS_INVALID;
    HfsPlusEntry record;
    int rc=(path[0]=='/' && !path[1])?root_record(v,&record):lookup(v,path,&record);
    if(rc!=IFS_FS_OK)return rc;
    /* An unset BSD mode is not a real POSIX mode. Do not fabricate one
     * for older Mac OS-created records. */
    if(!record.mode)return IFS_FS_UNSUPPORTED;
    *out=(IfsFsMetadataV2){.structure_bytes=sizeof(*out),.mask=IFS_FS_META_GET_MODE,
        .mode=record.mode,.owner=record.owner,.group=record.group};
    return IFS_FS_OK;
}
int hfsplus_catalog_stat_v2(const HfsPlusCatalogV2 *v,const char *path,
                            IfsFsEntryV2 *out)
{
    HfsPlusEntry record;
    int rc=lookup(v,path,&record);
    if(rc!=IFS_FS_OK)return rc;
    if(!path[1]) {
        record.utf16[0]=0;record.utf16[1]='/';
        record.utf16_length=1;
    }
    return emit(&record,out);
}
int hfsplus_catalog_list_v2(const HfsPlusCatalogV2 *v,const char *path,
                            uint32_t index,IfsFsEntryV2 *out)
{
    HfsPlusEntry directory;
    int rc=lookup(v,path,&directory);
    if(rc!=IFS_FS_OK)return rc;
    if(directory.type!=IFS_FS_ENTRY_DIRECTORY)return IFS_FS_INVALID;
    HfsPlusScan scan={.parent=directory.id,.wanted=index};
    rc=catalog_scan(v,&scan);
    if(rc==IFS_FS_NOT_FOUND)return IFS_FS_END;
    return rc==IFS_FS_OK?emit(&scan.result,out):rc;
}
int hfsplus_catalog_read_v2(const HfsPlusCatalogV2 *v,const char *path,
                            uint64_t offset,void *dst,uint32_t n)
{
    HfsPlusEntry file;
    int rc=lookup(v,path,&file);
    if(rc!=IFS_FS_OK)return rc;
    if(file.type!=IFS_FS_ENTRY_FILE)return IFS_FS_INVALID;
    rc=validate_extent_allocation(v,&file.fork);
    if(rc!=IFS_FS_OK)return rc;
    return fork_read_raw(v,&file.fork,offset,dst,n);
}

/* Map the actual data fork using the same validated allocation file and
 * pathname traversal as READ. An incomplete overflow fork is unsupported,
 * not EOF. Validate all physical allocations before returning any page. */
int hfsplus_catalog_map_v2(const HfsPlusCatalogV2 *v,const char *path,
                          uint64_t first,IfsFsExtentV2 *out,
                          uint32_t capacity,uint32_t *count)
{
    if(!count)return IFS_FS_INVALID;
    *count=0u;
    if(!v || !out || !capacity)return IFS_FS_INVALID;
    HfsPlusEntry file;
    int rc=lookup(v,path,&file);
    if(rc!=IFS_FS_OK)return rc;
    if(file.type!=IFS_FS_ENTRY_FILE)return IFS_FS_INVALID;
    rc=validate_extent_allocation(v,&file.fork);
    if(rc!=IFS_FS_OK)return rc;
    IfsFsExtentV2 complete[8]={{0}};
    uint32_t total=0u;
    uint64_t logical=0u;
    for(unsigned i=0u;i<8u;++i) {
        const HfsPlusExtentV2 *e=&file.fork.extents[i];
        if(!e->count)continue;
        for(unsigned j=0u;j<i;++j) {
            const HfsPlusExtentV2 *other=&file.fork.extents[j];
            if(other->count && e->start<other->start+other->count &&
               other->start<e->start+e->count)
                return IFS_FS_CORRUPT;
        }
        uint64_t physical=(uint64_t)e->start*v->block_bytes;
        uint64_t allocated=(uint64_t)e->count*v->block_bytes;
        if(physical>v->host->volume_bytes ||
           allocated>v->host->volume_bytes-physical)
            return IFS_FS_CORRUPT;
        if(logical==file.fork.length)continue;
        uint64_t length=file.fork.length-logical;
        if(length>allocated)length=allocated;
        if(total && complete[total-1u].physical_offset+
            complete[total-1u].length==physical)
            complete[total-1u].length+=length;
        else
            complete[total++]=(IfsFsExtentV2){
                .logical_offset=logical,.physical_offset=physical,.length=length};
        logical+=length;
    }
    if(logical!=file.fork.length)return IFS_FS_CORRUPT;
    if(first>=total)return IFS_FS_END;
    uint32_t take=total-(uint32_t)first;
    if(take>capacity)take=capacity;
    memcpy(out,complete+(uint32_t)first,take*sizeof(*out));
    *count=take;
    return IFS_FS_OK;
}

#ifndef IFS_HFSPLUS_FIRMWARE_BUILD
/* Native non-journalled root-file mutation profile. The formatter reserves
 * one 8 KiB catalogue leaf and its header; no B-tree splitting, overflow
 * extents, arbitrary foreign geometries, resource forks or overwrite are
 * advertised as qualified. All changes are staged before the first write.
 * Both volume headers are marked inconsistent before metadata mutation;
 * after a failed durable commit the volume is refused, not silently repaired.
 * This is a real HFS+/HFSX file allocator, not a test-image fixture codec. */
enum { MUT_BLOCK=4096u, MUT_NODE=8192u, MUT_RECORDS=128u, MUT_DATA=17u };
typedef struct {
    HfsPlusCatalogV2 *v;
    uint8_t header[512],tree[MUT_NODE],leaf[MUT_NODE],rebuilt[MUT_NODE];
    uint8_t data[MUT_DATA][MUT_BLOCK];
    uint32_t data_block[MUT_DATA],data_count;
    uint8_t *bitmap,*claimed;
    uint32_t cat_block,metadata_end;
    uint32_t record_count,root_record;
    uint32_t begin[MUT_RECORDS],end[MUT_RECORDS];
    int bitmap_changed,tree_changed;
} HfsPlusMutation;

static void mut16(uint8_t *p,uint32_t x)
{p[0]=(uint8_t)(x>>8);p[1]=(uint8_t)x;}
static void mut32(uint8_t *p,uint32_t x)
{mut16(p,x>>16);mut16(p+2,x);}
static void mut64(uint8_t *p,uint64_t x)
{mut32(p,(uint32_t)(x>>32));mut32(p+4,(uint32_t)x);}
static int mut_bit(const uint8_t *p,uint32_t i)
{return !!(p[i>>3u]&(uint8_t)(0x80u>>(i&7u)));}
static void mut_set(uint8_t *p,uint32_t i)
{p[i>>3u]|=(uint8_t)(0x80u>>(i&7u));}
static int mut_single_fork(const HfsPlusForkV2 *f,uint32_t start,uint32_t blocks)
{
    if(f->length!=(uint64_t)blocks*MUT_BLOCK || f->total_blocks!=blocks ||
       f->extents[0].start!=start || f->extents[0].count!=blocks)return 0;
    for(unsigned i=1u;i<8u;++i)
        if(f->extents[i].start || f->extents[i].count)return 0;
    return 1;
}
static uint32_t mut_record_value(const uint8_t *node,uint32_t begin)
{return begin+2u+rd16(node+begin);}
static int mut_key_cmp(const HfsPlusCatalogV2 *v,const uint8_t *a,const uint8_t *b)
{
    uint32_t pa=rd32(a+2),pb=rd32(b+2);
    if(pa!=pb)return pa<pb?-1:1;
    uint32_t na=rd16(a+6),nb=rd16(b+6);
    for(uint32_t i=0u;i<na && i<nb;++i) {
        uint32_t ca=rd16(a+8u+2u*i),cb=rd16(b+8u+2u*i);
        if(!v->case_sensitive) {
            if(ca>='A' && ca<='Z')ca+=32u;
            if(cb>='A' && cb<='Z')cb+=32u;
        }
        if(ca!=cb)return ca<cb?-1:1;
    }
    return na==nb?0:na<nb?-1:1;
}
static int mut_safe_name(const char *name)
{
    if(!name || !*name || !strcmp(name,".") || !strcmp(name,".."))return 0;
    size_t n=strlen(name);
    if(n>63u)return 0;
    for(size_t i=0;i<n;++i) {
        unsigned char c=(unsigned char)name[i];
        if(!((c>='A'&&c<='Z')||(c>='a'&&c<='z')||
             (c>='0'&&c<='9')||c=='.'||c=='_'||c=='-'))return 0;
    }
    return 1;
}
static int mut_claim(HfsPlusMutation *t,uint32_t b)
{
    if(b>=t->v->blocks || mut_bit(t->claimed,b) ||
       !mut_bit(t->bitmap,b))return IFS_FS_CORRUPT;
    mut_set(t->claimed,b);return IFS_FS_OK;
}
static void mut_free(HfsPlusMutation *t)
{
    if(!t)return;
    const IfsFsHostRuntimeV2 *h=t->v->host;
    if(t->bitmap)h->release(h->context,t->bitmap);
    if(t->claimed)h->release(h->context,t->claimed);
    h->release(h->context,t);
}
static int mut_load(HfsPlusCatalogV2 *v,HfsPlusMutation **out)
{
    *out=NULL;
    if(!v || !v->host || v->mutation_fault)return IFS_FS_IO_ERROR;
    const IfsFsHostRuntimeV2 *h=v->host;
    if((h->capabilities&IFS_FS_HOST_READ_ONLY) ||
       !(h->capabilities&IFS_FS_HOST_EXCLUSIVE) ||
       !(h->capabilities&IFS_FS_HOST_DURABLE))return IFS_FS_ACCESS;
    if(v->block_bytes!=MUT_BLOCK || v->node_bytes!=MUT_NODE ||
       v->node_count!=2u || v->leaf_head!=1u ||
       v->blocks<8192u || v->blocks>4194304u ||
       h->volume_bytes!=(uint64_t)v->blocks*MUT_BLOCK)return IFS_FS_ACCESS;
    uint32_t bitmap_blocks=(v->blocks+MUT_BLOCK*8u-1u)/(MUT_BLOCK*8u);
    uint32_t cat_block=bitmap_blocks+2u;
    if(!mut_single_fork(&v->bitmap_fork,1u,bitmap_blocks) ||
       !mut_single_fork(&v->catalog,cat_block,4u))return IFS_FS_ACCESS;
    HfsPlusMutation *t=h->allocate(h->context,sizeof(*t));
    if(!t)return IFS_FS_NO_MEMORY;
    memset(t,0,sizeof(*t));t->v=v;t->cat_block=cat_block;t->metadata_end=cat_block+4u;
    t->bitmap=h->allocate(h->context,v->bitmap_bytes);
    t->claimed=h->allocate(h->context,v->bitmap_bytes);
    int rc=IFS_FS_NO_MEMORY;
    if(!t->bitmap || !t->claimed)goto fail;
    memcpy(t->bitmap,v->bitmap,v->bitmap_bytes);
    memset(t->claimed,0,v->bitmap_bytes);
    uint8_t alternate[512];
    rc=read_bytes(h,1024u,t->header,sizeof(t->header));
    if(rc!=IFS_FS_OK)goto fail;
    rc=read_bytes(h,h->volume_bytes-1024u,alternate,sizeof(alternate));
    if(rc!=IFS_FS_OK)goto fail;
    HfsPlusForkV2 extents;
    parse_fork(t->header+192u,&extents);
    if(memcmp(t->header,alternate,sizeof(alternate)) ||
       rd32(t->header+4u)!=0x100u || rd32(t->header+8u)!=0x31302e30u ||
       rd32(t->header+12u) || rd32(t->header+36u) ||
       rd32(t->header+40u)!=MUT_BLOCK || rd32(t->header+44u)!=v->blocks ||
       rd32(t->header+64u)<16u ||
       !mut_single_fork(&extents,cat_block-1u,1u)) {
        rc=IFS_FS_ACCESS;goto fail;
    }
    for(unsigned i=352u;i<512u;++i)
        if(t->header[i]){rc=IFS_FS_ACCESS;goto fail;}
    for(uint32_t b=0;b<t->metadata_end;++b)
        if((rc=mut_claim(t,b))!=IFS_FS_OK)goto fail;
    if((rc=mut_claim(t,v->blocks-1u))!=IFS_FS_OK)goto fail;
    rc=read_bytes(h,(uint64_t)cat_block*MUT_BLOCK,t->tree,MUT_NODE);
    if(rc!=IFS_FS_OK)goto fail;
    rc=read_bytes(h,(uint64_t)cat_block*MUT_BLOCK+MUT_NODE,t->leaf,MUT_NODE);
    if(rc!=IFS_FS_OK)goto fail;
    if(t->tree[8]!=1u || rd16(t->tree+14u)!=1u ||
       rd32(t->tree+16u)!=1u || rd32(t->tree+24u)!=1u ||
       rd32(t->tree+28u)!=1u || rd16(t->tree+32u)!=MUT_NODE ||
       rd32(t->tree+36u)!=2u || rd32(t->tree+40u) ||
       t->leaf[8]!=0xffu || t->leaf[9]!=1u || rd32(t->leaf) ||
       rd32(t->leaf+4u)){rc=IFS_FS_ACCESS;goto fail;}
    t->record_count=rd16(t->leaf+10u);
    if(t->record_count<2u || t->record_count>MUT_RECORDS ||
       rd32(t->tree+20u)!=t->record_count){rc=IFS_FS_CORRUPT;goto fail;}
    uint32_t files=0u,threads=0u,roots=0u,root_threads=0u;
    for(uint32_t i=0u;i<t->record_count;++i) {
        uint32_t begin=rd16(t->leaf+MUT_NODE-2u*(i+1u));
        uint32_t end=rd16(t->leaf+MUT_NODE-2u*(i+2u));
        t->begin[i]=begin;t->end[i]=end;
        if(end>MUT_NODE-2u*(t->record_count+1u) ||
           (i && begin!=t->end[i-1u])){rc=IFS_FS_CORRUPT;goto fail;}
        HfsPlusEntry e;
        rc=decode_record(t->leaf,MUT_NODE,begin,end,&e);
        if(rc!=IFS_FS_OK)goto fail;
        if(i && mut_key_cmp(v,t->leaf+t->begin[i-1u],t->leaf+begin)>=0) {
            rc=IFS_FS_CORRUPT;goto fail;
        }
        for(uint32_t n=0u;n<e.utf16_length;++n)
            if(rd16(e.utf16+2u*n)<32u || rd16(e.utf16+2u*n)>126u) {
                rc=IFS_FS_ACCESS;goto fail;
            }
        const uint8_t *p=t->leaf+mut_record_value(t->leaf,begin);
        if(e.type==IFS_FS_ENTRY_DIRECTORY) {
            if(e.parent!=1u || e.id!=2u || ++roots!=1u) {
                rc=IFS_FS_ACCESS;goto fail;
            }
            t->root_record=i;
        } else if(e.type==IFS_FS_ENTRY_FILE) {
            if(e.parent!=2u || e.id>=rd32(t->header+64u) ||
               e.fork.length>(uint64_t)e.fork.total_blocks*MUT_BLOCK ||
               e.fork.total_blocks!=e.fork.length/MUT_BLOCK+
                    (e.fork.length%MUT_BLOCK!=0u)) {
                rc=IFS_FS_ACCESS;goto fail;
            }
            for(unsigned n=168u;n<248u;++n)
                if(p[n]){rc=IFS_FS_ACCESS;goto fail;}
            rc=validate_extent_allocation(v,&e.fork);
            if(rc!=IFS_FS_OK)goto fail;
            for(unsigned n=0u;n<8u;++n)
                for(uint32_t k=0u;k<e.fork.extents[n].count;++k)
                    if((rc=mut_claim(t,e.fork.extents[n].start+k))!=IFS_FS_OK)
                        goto fail;
            ++files;
        } else {
            uint32_t value=mut_record_value(t->leaf,begin);
            if(e.utf16_length || end-value<10u ||
               10u+2u*rd16(p+8u)>end-value){rc=IFS_FS_CORRUPT;goto fail;}
            if(rd16(p)==3u && e.parent==2u && rd32(p+4u)==1u)++root_threads;
            else if(rd16(p)==4u && e.parent>=16u &&
                    e.parent<rd32(t->header+64u) && rd32(p+4u)==2u)++threads;
            else {rc=IFS_FS_ACCESS;goto fail;}
        }
    }
    if(roots!=1u || root_threads!=1u || files!=threads ||
       files!=rd32(t->header+32u) ||
       rd32(t->leaf+mut_record_value(t->leaf,t->begin[t->root_record])+4u)!=files) {
        rc=IFS_FS_CORRUPT;goto fail;
    }
    /* Every live file must have exactly its matching native file thread. */
    for(uint32_t i=0u;i<t->record_count;++i) {
        uint8_t *key=t->leaf+t->begin[i];
        uint8_t *p=t->leaf+mut_record_value(t->leaf,t->begin[i]);
        if(rd16(p)!=2u)continue;
        unsigned matches=0u;
        for(uint32_t j=0u;j<t->record_count;++j) {
            uint8_t *other=t->leaf+t->begin[j];
            uint8_t *thread=t->leaf+mut_record_value(t->leaf,t->begin[j]);
            if(rd32(other+2u)==rd32(p+8u) && rd16(thread)==4u &&
               rd16(thread+8u)==rd16(key+6u) &&
               !memcmp(thread+10u,key+8u,2u*rd16(key+6u)))++matches;
        }
        if(matches!=1u){rc=IFS_FS_CORRUPT;goto fail;}
    }
    uint32_t free_blocks=0u;
    for(uint32_t b=0;b<v->blocks;++b) {
        if(mut_bit(t->bitmap,b)!=mut_bit(t->claimed,b)) {
            rc=IFS_FS_CORRUPT;goto fail;
        }
        if(!mut_bit(t->bitmap,b))++free_blocks;
    }
    if(free_blocks!=rd32(t->header+48u)){rc=IFS_FS_CORRUPT;goto fail;}
    *out=t;return IFS_FS_OK;
fail:
    mut_free(t);return rc;
}
int hfsplus_catalog_mutable_v2(HfsPlusCatalogV2 *v)
{
    HfsPlusMutation *t=NULL;
    int rc=mut_load(v,&t);
    mut_free(t);return rc;
}
static int mut_write_bytes(const IfsFsHostRuntimeV2 *h,uint64_t at,
                           const void *p,uint32_t bytes)
{return ifs_fs_host_write_bytes_v2(h,at,p,bytes);}
static int mut_commit(HfsPlusMutation *t)
{
    HfsPlusCatalogV2 *v=t->v;
    const IfsFsHostRuntimeV2 *h=v->host;
    if(h->cancelled && h->cancelled(h->context))return IFS_FS_BUSY;
    if(rd32(t->header+68u)==UINT32_MAX)return IFS_FS_NO_SPACE;
    mut32(t->header+68u,rd32(t->header+68u)+1u);
    uint8_t dirty[512];
    memcpy(dirty,t->header,sizeof(dirty));mut32(dirty+4u,0x800u);
    v->allocation_verified=0u;
    v->mutation_fault=1u;
    int rc=mut_write_bytes(h,h->volume_bytes-1024u,dirty,sizeof(dirty));
    if(rc==IFS_FS_OK)rc=mut_write_bytes(h,1024u,dirty,sizeof(dirty));
    if(rc!=IFS_FS_OK || h->flush(h->context)!=0)return IFS_FS_IO_ERROR;
    for(uint32_t i=0u;i<t->data_count;++i) {
        rc=mut_write_bytes(h,(uint64_t)t->data_block[i]*MUT_BLOCK,t->data[i],MUT_BLOCK);
        if(rc!=IFS_FS_OK)return rc;
    }
    if(t->bitmap_changed) {
        rc=mut_write_bytes(h,MUT_BLOCK,t->bitmap,v->bitmap_bytes);
        if(rc!=IFS_FS_OK)return rc;
    }
    if(t->tree_changed) {
        rc=mut_write_bytes(h,(uint64_t)t->cat_block*MUT_BLOCK,t->tree,MUT_NODE);
        if(rc!=IFS_FS_OK)return rc;
    }
    rc=mut_write_bytes(h,(uint64_t)t->cat_block*MUT_BLOCK+MUT_NODE,t->rebuilt,MUT_NODE);
    if(rc!=IFS_FS_OK || h->flush(h->context)!=0)return IFS_FS_IO_ERROR;
    rc=mut_write_bytes(h,h->volume_bytes-1024u,t->header,sizeof(t->header));
    if(rc!=IFS_FS_OK || h->flush(h->context)!=0)return IFS_FS_IO_ERROR;
    rc=mut_write_bytes(h,1024u,t->header,sizeof(t->header));
    if(rc!=IFS_FS_OK || h->flush(h->context)!=0)return IFS_FS_IO_ERROR;
    memcpy(v->bitmap,t->bitmap,v->bitmap_bytes);
    v->mutation_fault=0u;return IFS_FS_OK;
}
static uint32_t mut_key(uint8_t *p,uint32_t parent,const char *name)
{
    uint32_t n=(uint32_t)strlen(name);
    mut16(p,6u+2u*n);mut32(p+2u,parent);mut16(p+6u,n);
    for(uint32_t i=0;i<n;++i)mut16(p+8u+2u*i,(unsigned char)name[i]);
    return 8u+2u*n;
}
static int mut_add_record(HfsPlusMutation *t,const uint8_t *p,uint32_t bytes,
                          uint32_t *at,uint32_t *count)
{
    if(*count>=MUT_RECORDS || *at>MUT_NODE-2u*(*count+2u) ||
       bytes>MUT_NODE-2u*(*count+2u)-*at)return IFS_FS_NO_SPACE;
    mut16(t->rebuilt+MUT_NODE-2u*(*count+1u),*at);
    memcpy(t->rebuilt+*at,p,bytes);
    *at+=bytes;++*count;
    mut16(t->rebuilt+MUT_NODE-2u*(*count+1u),*at);
    return IFS_FS_OK;
}
int hfsplus_catalog_create_v2(HfsPlusCatalogV2 *v,const char *path,uint32_t type)
{
    if(!path || path[0]!='/' || !mut_safe_name(path+1u) ||
       type!=IFS_FS_ENTRY_FILE)return IFS_FS_UNSUPPORTED;
    HfsPlusMutation *t=NULL;
    int rc=mut_load(v,&t);
    if(rc!=IFS_FS_OK)return rc;
    HfsPlusEntry existing;
    rc=lookup(v,path,&existing);
    if(rc==IFS_FS_OK){rc=IFS_FS_EXISTS;goto done;}
    if(rc!=IFS_FS_NOT_FOUND)goto done;
    uint32_t id=rd32(t->header+64u);
    if(id==UINT32_MAX || rd32(t->header+32u)==UINT32_MAX) {
        rc=IFS_FS_NO_SPACE;goto done;
    }
    uint8_t file[384]={0},thread[160]={0};
    uint32_t file_key=mut_key(file,2u,path+1u);
    uint32_t thread_key=mut_key(thread,id,"");
    uint8_t *p=file+file_key;
    mut16(p,2u);mut16(p+2u,2u);mut32(p+8u,id);
    for(unsigned k=12u;k<=24u;k+=4u)mut32(p+k,rd32(t->header+20u));
    mut16(p+42u,0100644u);
    p=thread+thread_key;
    mut16(p,4u);mut32(p+4u,2u);
    uint32_t n=(uint32_t)strlen(path+1u);
    mut16(p+8u,n);
    for(uint32_t i=0u;i<n;++i)mut16(p+10u+2u*i,(unsigned char)path[i+1u]);
    t->rebuilt[8]=0xffu;t->rebuilt[9]=1u;
    uint32_t at=14u,count=0u;
    int added=0;
    for(uint32_t i=0u;i<t->record_count;++i) {
        uint8_t *record=t->leaf+t->begin[i];
        if(!added && mut_key_cmp(v,file,record)<0) {
            rc=mut_add_record(t,file,file_key+248u,&at,&count);
            if(rc!=IFS_FS_OK)goto done;
            added=1;
        }
        if(i==t->root_record) {
            uint8_t *folder=t->leaf+mut_record_value(t->leaf,t->begin[i]);
            mut32(folder+4u,rd32(folder+4u)+1u);
        }
        rc=mut_add_record(t,record,t->end[i]-t->begin[i],&at,&count);
        if(rc!=IFS_FS_OK)goto done;
    }
    if(!added) {
        rc=mut_add_record(t,file,file_key+248u,&at,&count);
        if(rc!=IFS_FS_OK)goto done;
    }
    rc=mut_add_record(t,thread,thread_key+10u+2u*n,&at,&count);
    if(rc!=IFS_FS_OK)goto done;
    mut16(t->rebuilt+10u,count);mut32(t->tree+20u,count);t->tree_changed=1;
    mut32(t->header+32u,rd32(t->header+32u)+1u);
    mut32(t->header+64u,id+1u);
    rc=mut_commit(t);
done:
    mut_free(t);return rc;
}
static int mut_allocate(HfsPlusMutation *t,HfsPlusForkV2 *fork,uint32_t *block)
{
    uint32_t free=rd32(t->header+48u);
    if(!free)return IFS_FS_NO_SPACE;
    uint32_t start=rd32(t->header+52u),limit=t->v->blocks-1u;
    if(start<t->metadata_end || start>=limit)start=t->metadata_end;
    for(uint32_t i=0u;i<limit-t->metadata_end;++i) {
        uint32_t b=t->metadata_end+(start-t->metadata_end+i)%(limit-t->metadata_end);
        if(mut_bit(t->bitmap,b))continue;
        unsigned n=0u;
        while(n<8u && fork->extents[n].count)++n;
        if(n && fork->extents[n-1u].start+fork->extents[n-1u].count==b)
            ++fork->extents[n-1u].count;
        else {
            if(n==8u)return IFS_FS_NO_SPACE;
            fork->extents[n]=(HfsPlusExtentV2){.start=b,.count=1u};
        }
        ++fork->total_blocks;
        mut_set(t->bitmap,b);t->bitmap_changed=1;
        mut32(t->header+48u,free-1u);mut32(t->header+52u,b+1u);
        *block=b;return IFS_FS_OK;
    }
    return IFS_FS_CORRUPT;
}
static int mut_existing_block(const HfsPlusForkV2 *fork,uint64_t logical,uint32_t *block)
{
    for(unsigned i=0u;i<8u;++i) {
        if(logical<fork->extents[i].count) {
            *block=fork->extents[i].start+(uint32_t)logical;return IFS_FS_OK;
        }
        logical-=fork->extents[i].count;
    }
    return IFS_FS_CORRUPT;
}
int hfsplus_catalog_append_v2(HfsPlusCatalogV2 *v,const char *path,uint64_t pos,
                              const void *data,uint32_t length)
{
    if(!path || path[0]!='/' || !mut_safe_name(path+1u) ||
       (!data && length) || length>65536u)return IFS_FS_UNSUPPORTED;
    HfsPlusMutation *t=NULL;
    int rc=mut_load(v,&t);
    if(rc!=IFS_FS_OK)return rc;
    HfsPlusEntry e;
    rc=lookup(v,path,&e);
    if(rc!=IFS_FS_OK)goto done;
    if(e.type!=IFS_FS_ENTRY_FILE || pos!=e.fork.length || pos>UINT64_MAX-length) {
        rc=IFS_FS_UNSUPPORTED;goto done;
    }
    if(!length){rc=IFS_FS_OK;goto done;}
    const uint8_t *source=data;
    uint32_t left=length;
    while(left) {
        if(t->data_count>=MUT_DATA){rc=IFS_FS_NO_SPACE;goto done;}
        uint32_t slot=t->data_count++,skip=(uint32_t)(pos%MUT_BLOCK),block;
        uint32_t take=MUT_BLOCK-skip;if(take>left)take=left;
        if(skip) {
            rc=mut_existing_block(&e.fork,pos/MUT_BLOCK,&block);
            if(rc!=IFS_FS_OK)goto done;
            rc=read_bytes(v->host,(uint64_t)block*MUT_BLOCK,t->data[slot],MUT_BLOCK);
        } else {
            rc=mut_allocate(t,&e.fork,&block);
        }
        if(rc!=IFS_FS_OK)goto done;
        t->data_block[slot]=block;
        memcpy(t->data[slot]+skip,source,take);
        pos+=take;source+=take;left-=take;
    }
    memcpy(t->rebuilt,t->leaf,MUT_NODE);
    uint8_t *fork=NULL;
    for(uint32_t i=0u;i<t->record_count;++i) {
        uint8_t *p=t->rebuilt+mut_record_value(t->rebuilt,t->begin[i]);
        if(rd16(p)==2u && rd32(p+8u)==e.id) {
            if(fork){rc=IFS_FS_CORRUPT;goto done;}
            fork=p+88u;
        }
    }
    if(!fork){rc=IFS_FS_CORRUPT;goto done;}
    mut64(fork,pos);mut32(fork+12u,e.fork.total_blocks);
    for(unsigned i=0u;i<8u;++i) {
        mut32(fork+16u+8u*i,e.fork.extents[i].start);
        mut32(fork+20u+8u*i,e.fork.extents[i].count);
    }
    rc=mut_commit(t);
done:
    mut_free(t);return rc;
}
#endif
