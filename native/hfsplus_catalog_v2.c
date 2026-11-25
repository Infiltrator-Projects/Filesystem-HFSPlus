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
    if((attrib&(1u<<13)) && !(attrib&(1u<<8)))
        return IFS_FS_UNSUPPORTED;
    parse_fork(vh+112,&v->bitmap_fork);
    parse_fork(vh+272,&v->catalog);
    if(!v->bitmap_fork.length || !v->catalog.length)
        return IFS_FS_UNSUPPORTED;
    v->bitmap_bytes=(v->blocks+7u)/8u;
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
    int found,unsupported,root_record;
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
            rc=(scan->name || scan->root_record) && scan->found?IFS_FS_OK:IFS_FS_NOT_FOUND;
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
static int lookup(const HfsPlusCatalogV2 *v,const char *path,HfsPlusEntry *e)
{
    if(!v || !path || *path!='/')return IFS_FS_INVALID;
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
