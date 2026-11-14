#include "evidence_codec.h"
#include <stdlib.h>
#include <string.h>

#define HEADER_MAX 16777216u
#define PAYLOAD_MAX 10485760u
#define BLOCK_MAX 4194303u
#define RECORD_MAX 4096u

static uint32_t crc_update(uint32_t c, const unsigned char *p, size_t n)
{
    while (n--) {
        c ^= *p++;
        for (unsigned i = 0; i < 8; ++i)
            c = (c >> 1) ^ ((c & 1) ? UINT32_C(0xedb88320) : 0);
    }
    return c;
}
uint32_t pgwt_codec_crc32(const void *p, size_t n)
{ return crc_update(UINT32_MAX, p, n) ^ UINT32_MAX; }

typedef struct { uint32_t h[8]; uint64_t bytes; unsigned char b[64]; size_t used; } sha;
static uint32_t rotr(uint32_t x, unsigned n) { return (x >> n) | (x << (32-n)); }
static void sha_block(sha *s, const unsigned char *p)
{
    static const uint32_t k[64] = {
        0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
        0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
        0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
        0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
        0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
        0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
        0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
        0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
    uint32_t w[64];
    for (unsigned i=0;i<16;++i)
        w[i]=(uint32_t)p[4*i]<<24 | (uint32_t)p[4*i+1]<<16 | (uint32_t)p[4*i+2]<<8 | p[4*i+3];
    for (unsigned i=16;i<64;++i) {
        uint32_t a=w[i-15], b=w[i-2];
        w[i]=w[i-16]+(rotr(a,7)^rotr(a,18)^(a>>3))+w[i-7]+(rotr(b,17)^rotr(b,19)^(b>>10));
    }
    uint32_t a=s->h[0],b=s->h[1],c=s->h[2],d=s->h[3],e=s->h[4],f=s->h[5],g=s->h[6],h=s->h[7];
    for (unsigned i=0;i<64;++i) {
        uint32_t t=h+(rotr(e,6)^rotr(e,11)^rotr(e,25))+((e&f)^(~e&g))+k[i]+w[i];
        uint32_t u=(rotr(a,2)^rotr(a,13)^rotr(a,22))+((a&b)^(a&c)^(b&c));
        h=g;g=f;f=e;e=d+t;d=c;c=b;b=a;a=t+u;
    }
    s->h[0]+=a;s->h[1]+=b;s->h[2]+=c;s->h[3]+=d;s->h[4]+=e;s->h[5]+=f;s->h[6]+=g;s->h[7]+=h;
}
static sha sha_init(void)
{
    sha s = {{0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19},0,{0},0};
    return s;
}
static void sha_update(sha *s, const unsigned char *p, size_t n)
{
    s->bytes += n;
    while (n) {
        size_t take = n < 64-s->used ? n : 64-s->used;
        memcpy(s->b+s->used,p,take);p+=take;n-=take;s->used+=take;
        if (s->used==64) { sha_block(s,s->b);s->used=0; }
    }
}
static void sha_end(sha *s, unsigned char out[32])
{
    uint64_t bits=s->bytes*8;
    s->b[s->used++]=0x80;
    if (s->used>56) { memset(s->b+s->used,0,64-s->used);sha_block(s,s->b);s->used=0; }
    memset(s->b+s->used,0,56-s->used);
    for (unsigned i=0;i<8;++i) s->b[63-i]=(unsigned char)(bits>>(8*i));
    sha_block(s,s->b);
    for (unsigned i=0;i<32;++i) out[i]=(unsigned char)(s->h[i/4]>>(24-8*(i%4)));
}
void pgwt_codec_sha256(const void *p, size_t n, unsigned char out[32])
{ sha s=sha_init();sha_update(&s,p,n);sha_end(&s,out); }
static void predecessor_hash(pgwt_codec_buffer b, size_t end, unsigned char out[32])
{
    sha s=sha_init();sha_update(&s,b.data,6);sha_update(&s,b.data+8,end-8);sha_end(&s,out);
}

typedef struct { const unsigned char *p; size_t n, at; bool bad; } reader;
static uint64_t get(reader *r, unsigned n)
{
    if (n>r->n-r->at) { r->bad=true;return 0; }
    uint64_t v=0;
    for (unsigned i=0;i<n;++i) v|=(uint64_t)r->p[r->at++]<<(8*i);
    return v;
}
static uint64_t at(const unsigned char *p, unsigned n)
{ reader r={p,n,0,false};return get(&r,n); }
static void put_at(unsigned char *p, uint64_t v, unsigned n)
{ for (unsigned i=0;i<n;++i) { p[i]=(unsigned char)v;v>>=8; } }
static bool boolean(reader *r)
{ uint64_t x=get(r,1);if (x>1) { r->bad=true; } return x==1; }
static void optional(reader *r, bool *present, uint64_t *value)
{ *present=boolean(r);if (*present) *value=get(r,8); }
static pgwt_core_event string(reader *r, size_t max)
{
    size_t n=(size_t)get(r,2);
    if (!n || n>max || n>r->n-r->at) { r->bad=true;return (pgwt_core_event){0}; }
    pgwt_core_event v={(const char *)r->p+r->at,n};r->at+=n;return v;
}
static void counters_read(reader *r, pgwt_evidence_counters *v)
{ v->known=boolean(r);if (v->known) for (unsigned i=0;i<15;++i) v->value[i]=get(r,8); }
static bool equal(pgwt_core_event a, pgwt_core_event b)
{ return a.length==b.length && (!a.length || (a.data && b.data && !memcmp(a.data,b.data,a.length))); }
static bool literal(pgwt_core_event a, const char *s)
{ return equal(a,(pgwt_core_event){s,strlen(s)}); }
static bool valid_string(pgwt_core_event e, size_t max)
{ return e.data && e.length && e.length<=max; }
static uint64_t uleb_read(reader *r)
{
    uint64_t v=0;
    for (unsigned i=0;i<10;++i) {
        unsigned b=(unsigned)get(r,1);
        if (r->bad || (i==9 && b!=1) || (i && b==0)) { r->bad=true;return 0; }
        v|=(uint64_t)(b&127)<<(7*i);
        if (!(b&128)) return v;
    }
    r->bad=true;return 0;
}

typedef struct { unsigned char *p; size_t n, cap, max; bool bad; } writer;
static void write_bytes(writer *w, const void *p, size_t n)
{
    if (w->bad) return;
    if (n>w->max-w->n) { w->bad=true;return; }
    size_t need=w->n+n;
    if (need>w->cap) {
        size_t cap=w->cap ? w->cap : 256;
        while (cap<need) { if (cap>w->max/2) { cap=w->max;break; } cap*=2; }
        unsigned char *q=realloc(w->p,cap);
        if (!q) { w->bad=true;return; } w->p=q;w->cap=cap;
    }
    if (n) memcpy(w->p+w->n,p,n);
    w->n=need;
}
static void put(writer *w, uint64_t v, unsigned n)
{ unsigned char b[8];put_at(b,v,n);write_bytes(w,b,n); }
static void string_write(writer *w, pgwt_core_event s)
{ put(w,s.length,2);write_bytes(w,s.data,s.length); }
static void optional_write(writer *w, bool has, uint64_t v)
{ put(w,has,1);if (has) put(w,v,8); }
static void counters_write(writer *w, const pgwt_evidence_counters *c)
{ put(w,c->known,1);if (c->known) for (unsigned i=0;i<15;++i) put(w,c->value[i],8); }
static void uleb_write(writer *w, uint64_t v)
{ do { unsigned b=(unsigned)(v&127);v>>=7;put(w,b|(v?128:0),1); } while (v); }
static void *array_grow(void *p, size_t n, size_t size)
{ if (n>SIZE_MAX/size) { return NULL; } return realloc(p,n*size); }

/* Header parsing is atomic. Mapping allocation is bounded by both count and
 * remaining header bytes. Names borrow the immutable original file buffer. */
static pgwt_codec_reason header(pgwt_codec_buffer b, pgwt_evidence_file *f,
                                 pgwt_codec_file_info *info)
{
    if (!b.data || b.size<48) return PGWT_CODEC_HEADER;
    const unsigned char *p=b.data;
    size_t h=(size_t)at(p+40,4);
    if (at(p,4)!=0x54574750 || at(p+4,2)!=5 || memcmp(p+36,"EVD1",4) ||
        h<73 || h>HEADER_MAX || h>b.size || at(p+44,2)!=1 || at(p+46,2)) return PGWT_CODEC_HEADER;
    uint32_t crc=crc_update(UINT32_MAX,p,6);
    crc=crc_update(crc,p+8,h-12)^UINT32_MAX;
    if (crc!=at(p+h-4,4)) return PGWT_CODEC_CRC;
    reader r={p,h-4,48,false};
    f->version=5;f->present=true;
    f->boot=string(&r,255);f->capture=string(&r,255);f->file_no=get(&r,8);
    f->has_previous=boolean(&r);
    if (f->has_previous) {
        f->previous=get(&r,8);info->previous_data_end=get(&r,8);
        for (unsigned i=0;i<32;++i) info->previous_sha256[i]=(unsigned char)get(&r,1);
    }
    f->qualified=boolean(&r);counters_read(&r,&f->baseline);
    size_t count=(size_t)get(&r,4);
    if (r.bad || count>65536 || count>(r.n-r.at)/8) return PGWT_CODEC_HEADER;
    pgwt_codec_mapping *map=count ? calloc(count,sizeof(*map)) : NULL;
    if (count && !map) return PGWT_CODEC_NONE;
    info->descriptor.mapping=map;info->descriptor.mapping_count=count;
    for (size_t i=0;i<count;++i) {
        map[i].id=(uint32_t)get(&r,4);map[i].role=(unsigned)get(&r,1);map[i].name=string(&r,1024);
        if (!map[i].id || map[i].id==UINT32_MAX || (i && map[i-1].id>=map[i].id) ||
            (map[i].role!=1 && map[i].role!=2) || literal(map[i].name,"$EXIT")) r.bad=true;
        for (size_t j=0;j<i;++j) if (equal(map[i].name,map[j].name)) r.bad=true;
    }
    if (r.bad || r.at!=r.n || (f->file_no==0 ? f->has_previous :
        (!f->has_previous || f->previous!=f->file_no-1))) return PGWT_CODEC_HEADER;
    info->header_size=h;info->data_end=h;info->bytes=b;
    info->descriptor.flags=(uint16_t)at(p+6,2);info->descriptor.pg_version=(uint32_t)at(p+8,4);
    info->descriptor.start_time_ns=at(p+12,8);info->descriptor.clock_offset_ns=at(p+20,8);
    info->descriptor.nominal_sample_period_ns=at(p+28,8);
    return PGWT_CODEC_RECORD; /* Internal success sentinel, not a fault. */
}
static const pgwt_codec_mapping *mapping_id(const pgwt_codec_descriptor *d, uint32_t id)
{
    size_t lo=0, hi=d->mapping_count;
    while (lo<hi) { size_t m=lo+(hi-lo)/2;if (d->mapping[m].id<id) lo=m+1;else hi=m; }
    return lo<d->mapping_count && d->mapping[lo].id==id ? &d->mapping[lo] : NULL;
}
static pgwt_core_event event_read(reader *r, const pgwt_codec_descriptor *d, bool exit)
{
    uint32_t id=(uint32_t)get(r,4);
    if (!id) return (pgwt_core_event){0};
    if (id==UINT32_MAX && exit) return (pgwt_core_event){"$EXIT",5};
    const pgwt_codec_mapping *m=mapping_id(d,id);
    if (!m || m->role!=1) { r->bad=true;return (pgwt_core_event){0}; }
    return m->name;
}
static bool event_id(const pgwt_codec_descriptor *d, pgwt_core_event e, bool exit, uint32_t *id)
{
    if (!e.data && !e.length) { *id=0;return true; }
    if (exit && literal(e,"$EXIT")) { *id=UINT32_MAX;return true; }
    for (size_t i=0;i<d->mapping_count;++i)
        if (d->mapping[i].role==1 && equal(d->mapping[i].name,e)) { *id=d->mapping[i].id;return true; }
    return false;
}
static void generation(reader *r, pgwt_evidence_record *v)
{ v->pid=(uint32_t)get(r,4);optional(r,&v->has_fork,&v->fork);if (!v->pid || (v->has_fork && v->fork>v->t)) r->bad=true; }
static void body_read(reader *r, unsigned kind, const pgwt_codec_descriptor *d, pgwt_evidence_record *v)
{
    v->t=get(r,8);
    switch (kind) {
    case 1:
        v->kind=PGWT_EVIDENCE_LIFE;generation(r,v);v->start=get(r,8);
        v->start_known=boolean(r);optional(r,&v->has_exit,&v->exit);
        if (v->start>v->t || (v->start_known ? (!v->has_fork || v->start!=v->fork) : v->has_fork) ||
            (v->has_exit && (v->exit<v->start || v->exit>v->t))) r->bad=true;
        break;
    case 2: {
        v->kind=PGWT_EVIDENCE_ATTACH_OK;generation(r,v);unsigned method=(unsigned)get(r,1);
        v->reread=method==1;if (method>1) r->bad=true;
        v->seed=event_read(r,d,false);v->state=event_read(r,d,false);counters_read(r,&v->counters);break;
    }
    case 3: v->kind=PGWT_EVIDENCE_FRONTIER;counters_read(r,&v->counters);break;
    case 4: {
        v->kind=PGWT_EVIDENCE_END;generation(r,v);unsigned reason=(unsigned)get(r,1);
        static const char *const names[]={"","detach","deescalate","stop"};
        if (!reason || reason>3) r->bad=true;
        else v->end_reason=(pgwt_core_event){names[reason],strlen(names[reason])};
        break;
    }
    case 5:
        v->kind=PGWT_EVIDENCE_VOID;counters_read(r,&v->counters);v->void_reason=string(r,4096);
        optional(r,&v->has_invalid_from,&v->invalid_from);break;
    default: v->kind=PGWT_EVIDENCE_OTHER;r->bad=true;break;
    }
    v->unsupported_shape=r->bad || r->at!=r->n;
}

/* Safe raw LZ4 decoder, including the normative end constraints. */
static bool lz4(const unsigned char *p, size_t n, unsigned char *out, size_t u)
{
    size_t i=0,o=0,last_match=0,last_literals=0;bool matched=false;
    while (i<n) {
        unsigned token=p[i++];size_t len=token>>4;
        if (len==15) { unsigned x;do { if (i==n) { return false; } x=p[i++];if (x>u || len>u-x) { return false; } len+=x; } while (x==255); }
        if (len>n-i || len>u-o) return false;
        memcpy(out+o,p+i,len);i+=len;o+=len;last_literals=len;
        if (i==n) return o==u && (!matched || (last_match<=u && u-last_match>=12)) && (u<5 || last_literals>=5);
        if (n-i<2) return false;
        size_t distance=p[i]|(size_t)p[i+1]<<8;i+=2;
        if (!distance || distance>o) return false;
        len=(token&15)+4;
        if ((token&15)==15) { unsigned x;do { if (i==n) { return false; } x=p[i++];if (x>u || len>u-x) { return false; } len+=x; } while (x==255); }
        if (len>u-o) return false;
        last_match=o;matched=true;
        for (size_t j=0;j<len;++j) { out[o]=out[o-distance];++o; }
    }
    return false;
}

typedef struct { pgwt_evidence_record r; pgwt_codec_transition meta; size_t off; bool omit; } row;
static pgwt_codec_reason payload(const unsigned char *p, size_t u, unsigned type, size_t n,
                                 const unsigned char *bh, const pgwt_codec_descriptor *d, row *rows)
{
    reader r={p,u,0,false};
    if (type==2) {
        if (u<8) return PGWT_CODEC_FRAMING;
        if (get(&r,2)!=1 || get(&r,2)) return PGWT_CODEC_SCHEMA;
        if (get(&r,4)!=n) return PGWT_CODEC_BOUNDS;
        /* First pass checks every envelope, before decoding any body. */
        for (size_t i=0;i<n;++i) {
            if (r.n-r.at<5) return PGWT_CODEC_FRAMING;
            rows[i].off=r.at;(void)get(&r,1);size_t len=(size_t)get(&r,4);
            if (r.bad || len<8 || len>65535) return PGWT_CODEC_BOUNDS;
            if (len>r.n-r.at) return PGWT_CODEC_FRAMING;
            r.at+=len;
        }
        if (r.at!=r.n) return PGWT_CODEC_FRAMING;
        for (size_t i=0;i<n;++i) {
            size_t off=rows[i].off;size_t len=(size_t)at(p+off+1,4);
            reader body={p+off+5,len,0,false};body_read(&body,p[off],d,&rows[i].r);
        }
        return PGWT_CODEC_NONE;
    }
    uint64_t prev=0;
    for (size_t i=0;i<n;++i) { rows[i].off=r.at;prev+=uleb_read(&r);rows[i].r.t=prev;rows[i].r.kind=PGWT_EVIDENCE_TRANS; }
    for (size_t i=0;i<n;++i) rows[i].r.pid=(uint32_t)get(&r,4);
    uint32_t old[RECORD_MAX]={0}, new_id[RECORD_MAX]={0};
    for (size_t i=0;i<n;++i) old[i]=(uint32_t)get(&r,4);
    if (type==0) {
        for (size_t i=0;i<n;++i) new_id[i]=(uint32_t)get(&r,4);
        for (size_t i=0;i<n;++i) rows[i].r.duration=uleb_read(&r);
    }
    for (size_t i=0;i<n;++i) rows[i].meta.query_id=get(&r,8);
    if (type==0) for (size_t i=0;i<n;++i) rows[i].meta.cpu_ns=uleb_read(&r);
    if (r.bad || r.at!=r.n) return PGWT_CODEC_ENCODING;
    if (rows[0].r.t!=at(bh,8) || rows[n-1].r.t!=at(bh+8,8)) return PGWT_CODEC_ENCODING;
    uint64_t weight=at(bh+32,8), maximum=weight;
    for (size_t i=0;i<n;++i) {
        pgwt_evidence_record *v=&rows[i].r;
        const pgwt_codec_mapping *a=mapping_id(d,old[i]);
        if (type==1) {
            if (i && v->t<rows[i-1].r.t) return PGWT_CODEC_ENCODING;
            if (i && v->t!=rows[i-1].r.t) weight=v->t-rows[i-1].r.t;
            if (weight>maximum) maximum=weight;
            rows[i].omit=true;
            v->unsupported_shape=!v->pid || (old[i] && (!a || a->role!=1));
        } else {
            const pgwt_codec_mapping *b=mapping_id(d,new_id[i]);
            bool marker=a && a->role==2 && old[i]==new_id[i] && !v->duration;
            rows[i].omit=marker;
            if (marker) continue;
            v->unsupported_shape=!v->pid || v->duration>v->t || old[i]==new_id[i] ||
                (old[i] && (!a || a->role!=1)) || (new_id[i] && new_id[i]!=UINT32_MAX && (!b || b->role!=1));
            if (a) v->old_event=a->name;
            if (new_id[i]==UINT32_MAX) v->new_event=(pgwt_core_event){"$EXIT",5};
            else if (b) v->new_event=b->name;
        }
    }
    if (type==1 && maximum!=at(bh+40,8)) return PGWT_CODEC_ENCODING;
    return PGWT_CODEC_NONE;
}

typedef struct { uint64_t time, offset; } index_entry;
static bool footer(pgwt_codec_buffer b, size_t off, const index_entry *index, size_t n)
{
    if ((n && !index) || n>(SIZE_MAX-4)/16 || b.size-off!=16*n+4 || at(b.data+b.size-4,4)!=n) return false;
    for (size_t i=0;i<n;++i)
        if (at(b.data+off+16*i,8)!=index[i].time || at(b.data+off+16*i+8,8)!=index[i].offset) return false;
    return true;
}
static int append_rows(pgwt_evidence_file *f, pgwt_codec_file_info *info, row *rows, size_t n, size_t off)
{
    size_t add=0;
    for (size_t i=0;i<n;++i) if (!rows[i].omit || rows[i].r.unsupported_shape) ++add;
    if (add>SIZE_MAX-f->record_count) return -1;
    size_t count=f->record_count+add;
    if (!add) return 0;
    void *p=array_grow((void *)f->records,count,sizeof(*f->records));
    if (!p) return -1;
    f->records=p;
    p=array_grow(info->positions,count,sizeof(*info->positions));
    if (!p) return -1;
    info->positions=p;
    p=array_grow((void *)info->descriptor.transitions,count,sizeof(pgwt_codec_transition));
    if (!p) return -1;
    info->descriptor.transitions=p;
    for (size_t i=0;i<n;++i) if (!rows[i].omit || rows[i].r.unsupported_shape) {
        size_t j=f->record_count++;
        ((pgwt_evidence_record *)f->records)[j]=rows[i].r;
        info->positions[j]=(pgwt_codec_position){off,rows[i].off};
        ((pgwt_codec_transition *)info->descriptor.transitions)[j]=rows[i].meta;
    }
    return 0;
}
/* Independent scan: structural faults do not end byte validation. */
static int scan(pgwt_evidence_file *f, pgwt_codec_file_info *info, pgwt_codec_stop *stop)
{
    pgwt_codec_buffer b=info->bytes;size_t off=info->header_size;
    uint32_t preceding=(uint32_t)at(b.data+off-4,4);
    index_entry *index=NULL;row *rows=calloc(RECORD_MAX,sizeof(*rows));
    if (!rows) return -1;
    pgwt_codec_reason fault=PGWT_CODEC_NONE;size_t fault_record=0,fault_block=0;
    while (off<b.size) {
        if (footer(b,off,index,info->block_count)) { info->descriptor.footer=true;break; }
        size_t remaining=b.size-off;const unsigned char *bh=b.data+off;
        pgwt_codec_reason reason=PGWT_CODEC_NONE;
        if (remaining<64) reason=PGWT_CODEC_FRAMING;
        size_t n=0,s=0,u=0;unsigned type=0,codec=0;
        if (!reason) {
            n=(size_t)at(bh+16,4);s=(size_t)at(bh+20,4);u=(size_t)at(bh+24,4);
            type=(unsigned)at(bh+28,2);codec=bh[60];
            if (!n || n>RECORD_MAX || !s || s>PAYLOAD_MAX || !u || u>PAYLOAD_MAX || info->block_count>=BLOCK_MAX)
                reason=PGWT_CODEC_BOUNDS;
            else if (at(bh+48,8)!=off || at(bh+56,4)!=preceding) reason=PGWT_CODEC_LINK;
            else if (type>2 || codec>1 || (type==2 && codec) || at(bh+30,2) || bh[61] || bh[62] || bh[63]) reason=PGWT_CODEC_SCHEMA;
            else if (s>remaining-64 || remaining-64-s<4) reason=PGWT_CODEC_FRAMING;
            else if (pgwt_codec_crc32(bh,64+s)!=at(bh+64+s,4)) reason=PGWT_CODEC_CRC;
            else if ((codec==0 && s!=u) || (type!=1 && (at(bh+32,8) || at(bh+40,8)))) reason=PGWT_CODEC_ENCODING;
        }
        pgwt_codec_code terminal_code=reason==PGWT_CODEC_CRC ? PGWT_CODEC_BAD_CRC :
            reason==PGWT_CODEC_FRAMING ? PGWT_CODEC_TRUNCATED : PGWT_CODEC_INVALID_BLOCK;
        unsigned char *decoded=NULL;
        if (!reason && codec) {
            decoded=malloc(u);
            if (!decoded) { free(index);free(rows);return -1; }
            if (!lz4(bh+64,s,decoded,u)) reason=PGWT_CODEC_ENCODING;
        }
        if (!reason) {
            memset(rows,0,RECORD_MAX*sizeof(*rows));
            reason=payload(decoded ? decoded : bh+64,u,type,n,bh,&info->descriptor,rows);
            if (!reason && type==1 && info->has_samples &&
                (rows[0].r.t<info->last_sample_time ||
                 (rows[0].r.t==info->last_sample_time && at(bh+32,8)!=info->last_sample_weight)))
                reason=PGWT_CODEC_ENCODING;
        }
        free(decoded);
        if (reason) {
            *stop=(pgwt_codec_stop){.input_file=stop->input_file,.block_offset=off,
                .validated_end=info->data_end,.reason=reason,.code=terminal_code};
            free(index);free(rows);return 1;
        }
        if (!fault) for (size_t i=0;i<n;++i) if (rows[i].r.unsupported_shape) {
            /* Unqualified recognizable certificates are ignored by replay. */
            pgwt_evidence_kind k=rows[i].r.kind;
            if (!f->qualified && (k==PGWT_EVIDENCE_ATTACH_OK || k==PGWT_EVIDENCE_FRONTIER || k==PGWT_EVIDENCE_END)) continue;
            fault=PGWT_CODEC_RECORD;fault_block=off;fault_record=rows[i].off;break;
        }
        if (append_rows(f,info,rows,n,off)) { free(index);free(rows);return -1; }
        if (type==1) {
            uint64_t weight=at(bh+32,8);
            for (size_t i=1;i<n;++i) if (rows[i].r.t!=rows[i-1].r.t) weight=rows[i].r.t-rows[i-1].r.t;
            info->has_samples=true;info->last_sample_time=rows[n-1].r.t;info->last_sample_weight=weight;
        }
        index_entry *grown=array_grow(index,info->block_count+1,sizeof(*index));
        if (!grown) { free(index);free(rows);return -1; }index=grown;
        index[info->block_count++]=(index_entry){at(bh,8),off};
        preceding=(uint32_t)at(bh+64+s,4);off+=68+s;info->data_end=off;

    }
    free(index);free(rows);
    if (fault) { *stop=(pgwt_codec_stop){.input_file=stop->input_file,.block_offset=fault_block,
        .record_offset=fault_record,.validated_end=info->data_end,.reason=fault,.code=PGWT_CODEC_RF,.has_record_offset=true};return 1; }
    return 0;
}

static void file_free(pgwt_evidence_file *f, pgwt_codec_file_info *info)
{
    free((void *)f->records);free((void *)info->descriptor.mapping);
    free((void *)info->descriptor.transitions);free(info->positions);
}
void pgwt_codec_decoded_free(pgwt_codec_decoded *out)
{
    if (!out) return;
    if (out->files && out->info)
        for (size_t i=0;i<out->file_count;++i) file_free(&out->files[i],&out->info[i]);
    for (size_t i=0;i<out->stop_count;++i) free(out->stops[i].witnesses);
    for (size_t i=0;i<out->diagnostic_count;++i) free(out->diagnostics[i].witnesses);
    for (size_t i=0;i<out->delivery_count;++i) free(out->deliveries[i].checks);
    for (size_t i=0;i<out->group_count;++i) free(out->groups[i].sources);
    for (size_t i=0;i<out->capture_count;++i) {
        pgwt_codec_capture *c=&out->captures[i];
        free(c->boots);free(c->witnesses);
        free(c->replay.frontiers);free(c->replay.revoked_frontiers);
    }
    free(out->files);free(out->info);free(out->stops);free(out->diagnostics);
    free(out->deliveries);free(out->groups);free(out->captures);memset(out,0,sizeof(*out));
}
static bool same_mapping(const pgwt_codec_descriptor *a, const pgwt_codec_descriptor *b)
{
    if (a->mapping_count!=b->mapping_count) return false;
    for (size_t i=0;i<a->mapping_count;++i)
        if (a->mapping[i].id!=b->mapping[i].id || a->mapping[i].role!=b->mapping[i].role || !equal(a->mapping[i].name,b->mapping[i].name)) return false;
    return true;
}
typedef struct { pgwt_evidence_file file; pgwt_codec_file_info info; pgwt_codec_stop terminal; } copy_state;

static int event_order(pgwt_core_event a, pgwt_core_event b)
{
    size_t n=a.length<b.length ? a.length : b.length;
    int c=n ? memcmp(a.data,b.data,n) : 0;
    return c ? c : (a.length>b.length)-(a.length<b.length);
}
static int identity_order(pgwt_codec_identity a, pgwt_codec_identity b)
{
    int c=event_order(a.boot,b.boot);if (c) return c;
    c=event_order(a.capture,b.capture);if (c) return c;
    return (a.file_no>b.file_no)-(a.file_no<b.file_no);
}
static pgwt_codec_identity identity(const pgwt_evidence_file *f)
{ return (pgwt_codec_identity){f->boot,f->capture,f->file_no}; }
static int group_order(const void *a, const void *b)
{ return identity_order(((const pgwt_codec_group *)a)->identity,((const pgwt_codec_group *)b)->identity); }
static int delivery_order(const void *a, const void *b)
{
    const pgwt_codec_delivery *x=a,*y=b;
    if (x->bytes.size!=y->bytes.size) return x->bytes.size<y->bytes.size ? -1 : 1;
    int c=memcmp(x->sha256,y->sha256,32);
    return c ? c : (x->bytes.size ? memcmp(x->bytes.data,y->bytes.data,x->bytes.size) : 0);
}
static int point_order(const pgwt_codec_stop *a, const pgwt_codec_stop *b)
{
    if (a->block_offset!=b->block_offset) return a->block_offset<b->block_offset ? -1 : 1;
    if (a->has_record_offset!=b->has_record_offset) return a->has_record_offset ? 1 : -1;
    if (a->has_record_offset && a->record_offset!=b->record_offset) return a->record_offset<b->record_offset ? -1 : 1;
    return 0;
}
static int failure_order(const void *a, const void *b)
{
    const pgwt_codec_stop *x=a,*y=b;
    if (x->group!=y->group) {
        if (x->group==SIZE_MAX) return -1;
        if (y->group==SIZE_MAX) return 1;
        return x->group<y->group ? -1 : 1;
    }
    int c=point_order(x,y);if (c) return c;
    return (x->code>y->code)-(x->code<y->code);
}
static int witness_order(const void *a, const void *b)
{
    const pgwt_codec_witness *x=a,*y=b;
    if (x->first!=y->first) return x->first<y->first ? -1 : 1;
    return (x->second>y->second)-(x->second<y->second);
}
static int add_index(size_t **array, size_t *count, size_t value)
{
    void *p=array_grow(*array,*count+1,sizeof(**array));if (!p) return -1;
    *array=p;(*array)[(*count)++]=value;return 0;
}
static int add_failure(pgwt_codec_stop **array, size_t *count, pgwt_codec_stop s, pgwt_codec_witness w)
{
    size_t i=0;
    for (;i<*count;++i) if (!failure_order(&(*array)[i],&s)) break;
    if (i==*count) {
        void *p=array_grow(*array,*count+1,sizeof(**array));if (!p) return -1;
        *array=p;s.witnesses=NULL;s.witness_count=0;(*array)[(*count)++]=s;
    }
    pgwt_codec_stop *v=&(*array)[i];
    for (size_t j=0;j<v->witness_count;++j)
        if (!witness_order(&v->witnesses[j],&w)) return 0;
    void *p=array_grow(v->witnesses,v->witness_count+1,sizeof(w));if (!p) return -1;
    v->witnesses=p;v->witnesses[v->witness_count++]=w;
    qsort(v->witnesses,v->witness_count,sizeof(w),witness_order);return 0;
}
static int observe(pgwt_codec_decoded *out, pgwt_codec_stop **cs, size_t *nc,
                   pgwt_codec_stop s, pgwt_codec_witness w, bool eligible)
{
    if (add_failure(&out->diagnostics,&out->diagnostic_count,s,w)) return -1;
    return eligible ? add_failure(cs,nc,s,w) : 0;
}
static bool same_header(const copy_state *a, const copy_state *b)
{
    size_t h=a->info.header_size;
    return h==b->info.header_size && !memcmp(a->info.bytes.data,b->info.bytes.data,6) &&
        !memcmp(a->info.bytes.data+8,b->info.bytes.data+8,h-8);
}
static pgwt_codec_link_check check_link(size_t predecessor, const copy_state *p, const copy_state *s, bool clean)
{
    const pgwt_codec_file_info *info=&s->info;
    uint64_t D=info->previous_data_end;
    pgwt_codec_link_check c={.predecessor=predecessor,.validated_end=p->info.data_end,.clean=clean};
    c.static_match=equal(p->file.boot,s->file.boot) && equal(p->file.capture,s->file.capture) &&
        p->file.file_no==s->file.previous && p->file.qualified==s->file.qualified &&
        same_mapping(&p->info.descriptor,&info->descriptor);
    size_t off=p->info.header_size;c.boundary=D==off;
    while (off<p->info.data_end) {
        off+=68+(size_t)at(p->info.bytes.data+off+20,4);
        if (D==off) c.boundary=true;
    }
    if (c.boundary) {
        unsigned char digest[32];predecessor_hash(p->info.bytes,(size_t)D,digest);
        c.digest_match=memcmp(digest,info->previous_sha256,32)==0;
    }
    c.exact_end=D==p->info.data_end;c.extension=c.boundary && D<p->info.data_end;
    c.witness=c.static_match && c.boundary && c.digest_match && clean && c.exact_end;
    c.seal_proof=c.static_match && c.boundary && c.digest_match && c.extension;
    return c;
}
static bool ignored_certificate(const pgwt_evidence_file *f, const pgwt_evidence_record *r)
{
    return !f->qualified && (r->kind==PGWT_EVIDENCE_ATTACH_OK ||
        r->kind==PGWT_EVIDENCE_FRONTIER || r->kind==PGWT_EVIDENCE_END);
}
/* The codec owns physical reconciliation and byte positions. Replay alone owns
 * semantic decisions, including the running baseline at successor descriptors. */
static int capture_replay(pgwt_codec_decoded *out)
{
    if (!out->file_count) return 0;
    pgwt_evidence_input in={.files=out->files,.file_count=out->file_count,
        .capture=out->files[0].capture,.pid=1};
    pgwt_core_query q={.event={"codec-report",12},.from=0,.to=1};
    pgwt_evidence_output output={0};pgwt_evidence_report report={0};
    if (pgwt_evidence_replay_report(&in,&q,&output,&report,NULL)!=PGWT_REPLAY_OK) return -1;
    pgwt_evidence_output_free(&output);
    for (size_t i=0;i<report.capture_count;++i) {
        pgwt_evidence_capture_report *r=&report.captures[i];
        for (size_t j=0;j<out->capture_count;++j) if (equal(out->captures[j].capture,r->capture)) {
            pgwt_codec_capture *c=&out->captures[j];c->has_replay=true;c->replay=*r;
            r->frontiers=NULL;r->revoked_frontiers=NULL;
            if (c->replay.stop.stopped) {
                c->failed=true;size_t f=c->replay.stop.file_index;
                for (size_t g=0;g<out->group_count;++g)
                    if (out->groups[g].file_index==f) c->replay_group=g;
                if (!c->replay.stop.descriptor)
                    c->replay_position=out->info[f].positions[c->replay.stop.record_index];
            }
        }
    }
    pgwt_evidence_report_free(&report);return 0;
}
int pgwt_codec_decode(const pgwt_codec_buffer *buffers, size_t count, pgwt_codec_decoded *out)
{
    if (!out) return -1;
    memset(out,0,sizeof(*out));
    if ((count && !buffers) || count>SIZE_MAX/sizeof(copy_state) ||
        count>SIZE_MAX/sizeof(pgwt_codec_delivery) || count>SIZE_MAX/sizeof(pgwt_codec_group) ||
        count>SIZE_MAX/sizeof(pgwt_codec_capture)) return -1;
    if (!count) return 0;
    copy_state *states=NULL;pgwt_codec_stop *candidates=NULL;size_t candidate_count=0;
    out->deliveries=calloc(count,sizeof(*out->deliveries));
    out->groups=calloc(count,sizeof(*out->groups));out->captures=calloc(count,sizeof(*out->captures));
    out->files=calloc(count,sizeof(*out->files));out->info=calloc(count,sizeof(*out->info));
    if (!out->deliveries || !out->groups || !out->captures || !out->files || !out->info) goto error;
    for (size_t i=0;i<count;++i) {
        if (buffers[i].size && !buffers[i].data) goto error;
        out->deliveries[i].bytes=buffers[i];out->deliveries[i].group=SIZE_MAX;
        pgwt_codec_sha256(buffers[i].data,buffers[i].size,out->deliveries[i].sha256);
    }
    qsort(out->deliveries,count,sizeof(*out->deliveries),delivery_order);
    for (size_t i=0;i<count;++i) {
        if (i && !delivery_order(&out->deliveries[i],&out->deliveries[i-1])) continue;
        out->deliveries[out->delivery_count++]=out->deliveries[i];
    }
    size_t n=out->delivery_count;
    states=calloc(n,sizeof(*states));if (!states) goto error;
    for (size_t i=0;i<n;++i) {
        copy_state *s=&states[i];pgwt_codec_delivery *d=&out->deliveries[i];
        pgwt_codec_reason hr=header(d->bytes,&s->file,&s->info);
        if (hr==PGWT_CODEC_NONE) goto error;
        if (hr!=PGWT_CODEC_RECORD) {
            pgwt_codec_stop stop={.input_file=i,.reason=hr,.code=PGWT_CODEC_INVALID_HEADER,.group=SIZE_MAX};
            if (observe(out,&candidates,&candidate_count,stop,(pgwt_codec_witness){i,SIZE_MAX},false)) goto error;
            out->unassigned_header_failure=true;continue;
        }
        if (scan(&s->file,&s->info,&s->terminal)<0) goto error;
        d->validated_end=s->info.data_end;
        d->clean=!s->terminal.reason || s->terminal.reason==PGWT_CODEC_RECORD;
        d->previous_data_end=s->info.previous_data_end;memcpy(d->previous_sha256,s->info.previous_sha256,32);
        pgwt_codec_identity key=identity(&s->file);size_t g=0;
        for (;g<out->group_count;++g) if (!identity_order(out->groups[g].identity,key)) break;
        if (g==out->group_count) { out->groups[g].identity=key;out->groups[g].file_index=SIZE_MAX;++out->group_count; }
    }
    qsort(out->groups,out->group_count,sizeof(*out->groups),group_order);
    for (size_t i=0;i<n;++i) if (states[i].info.header_size) {
        for (size_t g=0;g<out->group_count;++g)
            if (!identity_order(out->groups[g].identity,identity(&states[i].file))) out->deliveries[i].group=g;
    }
    /* Sample ticks are a stream even across rotation (CODEC §6). Carry
     * their state only through an independently clean, exact digest witness,
     * never a chosen arrival or a union assembled from partial deliveries.
     * Groups are in ordinal order, so empty intervening files carry it too. */
    for (size_t g=0;g<out->group_count;++g) for (size_t i=0;i<n;++i) {
        pgwt_codec_delivery *d=&out->deliveries[i];copy_state *s=&states[i];
        if (d->group!=g || !s->file.file_no) continue;
        for (size_t j=0;j<n;++j) {
            if (out->deliveries[j].group==SIZE_MAX || !states[j].info.has_samples ||
                !equal(s->file.boot,states[j].file.boot) || !equal(s->file.capture,states[j].file.capture) ||
                states[j].file.file_no!=s->file.file_no-1) continue;
            pgwt_codec_link_check c=check_link(j,&states[j],s,out->deliveries[j].clean);
            if (!c.witness) continue;
            free((void *)s->file.records);s->file.records=NULL;s->file.record_count=0;
            free(s->info.positions);s->info.positions=NULL;
            free((void *)s->info.descriptor.transitions);s->info.descriptor.transitions=NULL;
            s->info.block_count=0;s->info.data_end=s->info.header_size;s->info.descriptor.footer=false;
            s->info.has_samples=true;s->info.last_sample_time=states[j].info.last_sample_time;
            s->info.last_sample_weight=states[j].info.last_sample_weight;
            memset(&s->terminal,0,sizeof(s->terminal));
            if (scan(&s->file,&s->info,&s->terminal)<0) goto error;
            d->validated_end=s->info.data_end;d->clean=!s->terminal.reason || s->terminal.reason==PGWT_CODEC_RECORD;
            break;
        }
    }
    /* All headers, not just reachable roots, establish the capture's boot set. */
    for (size_t g=0;g<out->group_count;++g) {
        pgwt_codec_identity id=out->groups[g].identity;size_t c=0;
        for (;c<out->capture_count;++c) if (equal(out->captures[c].capture,id.capture)) break;
        if (c==out->capture_count) {
            out->captures[c].capture=id.capture;out->captures[c].replay_group=SIZE_MAX;++out->capture_count;
        }
        pgwt_codec_capture *cap=&out->captures[c];bool seen=false;
        for (size_t j=0;j<cap->boot_count;++j) if (equal(cap->boots[j],id.boot)) seen=true;
        if (!seen) {
            void *p=array_grow(cap->boots,cap->boot_count+1,sizeof(*cap->boots));if (!p) goto error;
            cap->boots=p;cap->boots[cap->boot_count++]=id.boot;
        }
    }
    /* Insertion sort capture IDs; boot order already follows canonical groups. */
    for (size_t i=1;i<out->capture_count;++i) {
        pgwt_codec_capture c=out->captures[i];size_t j=i;
        while (j && event_order(out->captures[j-1].capture,c.capture)>0) { out->captures[j]=out->captures[j-1];--j; }
        out->captures[j]=c;
    }
    for (size_t c=0;c<out->capture_count;++c) {
        pgwt_codec_capture *cap=&out->captures[c];cap->ambiguous=cap->boot_count>1;cap->failed=cap->ambiguous;
        for (size_t i=0;i<n;++i) if (out->deliveries[i].group!=SIZE_MAX && equal(states[i].file.capture,cap->capture))
            if (add_index(&cap->witnesses,&cap->witness_count,i)) goto error;
    }
    for (size_t i=0;i<n;++i) {
        copy_state *s=&states[i];pgwt_codec_delivery *d=&out->deliveries[i];if (d->group==SIZE_MAX) continue;
        pgwt_codec_witness w={i,SIZE_MAX};
        if (!d->clean) {
            pgwt_codec_stop stop=s->terminal;stop.input_file=i;stop.group=d->group;
            if (observe(out,&candidates,&candidate_count,stop,w,false)) goto error;
        }
        for (size_t r=0;r<s->file.record_count;++r) if (s->file.records[r].unsupported_shape) {
            pgwt_codec_position p=s->info.positions[r];
            pgwt_codec_stop stop={.input_file=i,.block_offset=p.block_offset,.record_offset=p.record_offset,
                .validated_end=d->validated_end,.reason=PGWT_CODEC_RECORD,.code=PGWT_CODEC_RF,
                .has_record_offset=true,.group=d->group};
            if (observe(out,&candidates,&candidate_count,stop,w,!ignored_certificate(&s->file,&s->file.records[r]))) goto error;
        }
    }
    for (size_t g=0;g<out->group_count;++g) {
        size_t E=0;bool hc=false,clean=false;
        for (size_t i=0;i<n;++i) if (out->deliveries[i].group==g) {
            if (out->deliveries[i].validated_end>E) E=out->deliveries[i].validated_end;
            for (size_t j=i+1;j<n;++j) if (out->deliveries[j].group==g) {
                pgwt_codec_stop stop={.input_file=i,.group=g,.reason=PGWT_CODEC_CRC,.code=PGWT_CODEC_BC};
                bool conflict=false;
                if (!same_header(&states[i],&states[j])) { hc=true;stop.code=PGWT_CODEC_HC;stop.reason=PGWT_CODEC_HEADER;conflict=true; }
                else {
                    size_t off=states[i].info.header_size;
                    while (off<states[i].info.data_end && off<states[j].info.data_end) {
                        size_t a=68+(size_t)at(states[i].info.bytes.data+off+20,4);
                        size_t b=68+(size_t)at(states[j].info.bytes.data+off+20,4);
                        if (a!=b || memcmp(states[i].info.bytes.data+off,states[j].info.bytes.data+off,a)) {
                            stop.block_offset=off;stop.validated_end=off;conflict=true;break;
                        }
                        off+=a;
                    }
                }
                if (conflict && observe(out,&candidates,&candidate_count,stop,(pgwt_codec_witness){i,j},true)) goto error;
            }
        }
        pgwt_codec_group *group=&out->groups[g];
        for (size_t i=0;i<n;++i) if (out->deliveries[i].group==g && out->deliveries[i].validated_end==E) {
            if (!hc && add_index(&group->sources,&group->source_count,i)) goto error;
            if (out->deliveries[i].clean) clean=true;
        }
        if (!clean) {
            pgwt_codec_code code=PGWT_CODEC_RF;
            for (size_t i=0;i<n;++i) if (out->deliveries[i].group==g && out->deliveries[i].validated_end==E && states[i].terminal.code<code) code=states[i].terminal.code;
            for (size_t i=0;i<n;++i) if (out->deliveries[i].group==g && out->deliveries[i].validated_end==E && states[i].terminal.code==code) {
                pgwt_codec_stop stop=states[i].terminal;stop.input_file=i;stop.group=g;
                if (add_failure(&candidates,&candidate_count,stop,(pgwt_codec_witness){i,SIZE_MAX})) goto error;
            }
        }
    }
    /* Local linkage is independent of any earlier capture cutoff/ambiguity. */
    for (size_t i=0;i<n;++i) {
        pgwt_codec_delivery *d=&out->deliveries[i];copy_state *s=&states[i];
        if (d->group==SIZE_MAX || !s->file.file_no) continue;
        bool witnessed=false,sealed=false;
        for (size_t j=0;j<n;++j) if (out->deliveries[j].group!=SIZE_MAX &&
            equal(s->file.boot,states[j].file.boot) && equal(s->file.capture,states[j].file.capture) &&
            states[j].file.file_no==s->file.file_no-1) {
            pgwt_codec_link_check c=check_link(j,&states[j],s,out->deliveries[j].clean);
            void *p=array_grow(d->checks,d->check_count+1,sizeof(c));if (!p) goto error;
            d->checks=p;d->checks[d->check_count++]=c;witnessed|=c.witness;sealed|=c.seal_proof;
            if (c.seal_proof) {
                pgwt_codec_stop stop={.input_file=j,.block_offset=(size_t)d->previous_data_end,
                    .validated_end=(size_t)d->previous_data_end,.reason=PGWT_CODEC_LINK,.code=PGWT_CODEC_SC,.group=out->deliveries[j].group};
                if (observe(out,&candidates,&candidate_count,stop,(pgwt_codec_witness){j,i},true)) goto error;
            }
        }
        d->link=sealed ? PGWT_CODEC_SEAL_CONFLICT : witnessed ? PGWT_CODEC_WITNESSED : PGWT_CODEC_UNAVAILABLE;
        if (!sealed && !witnessed) {
            pgwt_codec_stop stop={.input_file=i,.reason=PGWT_CODEC_LINK,.code=PGWT_CODEC_PU,.group=d->group};
            if (observe(out,&candidates,&candidate_count,stop,(pgwt_codec_witness){i,SIZE_MAX},true)) goto error;
        }
    }
    if (candidate_count) qsort(candidates,candidate_count,sizeof(*candidates),failure_order);
    for (size_t i=0;i<candidate_count;++i) {
        if (i && candidates[i].group==candidates[i-1].group) continue;
        for (size_t w=0;w<candidates[i].witness_count;++w)
            if (add_failure(&out->stops,&out->stop_count,candidates[i],candidates[i].witnesses[w])) goto error;
    }
    if (out->diagnostic_count) qsort(out->diagnostics,out->diagnostic_count,sizeof(*out->diagnostics),failure_order);
    for (size_t g=0;g<out->group_count;++g) {
        pgwt_codec_group *group=&out->groups[g];pgwt_codec_identity id=group->identity;
        pgwt_codec_capture *cap=NULL;
        for (size_t c=0;c<out->capture_count;++c) if (equal(out->captures[c].capture,id.capture)) cap=&out->captures[c];
        bool reachable=group->source_count>0 && !cap->ambiguous;
        uint64_t cursor=0;
        for (size_t k=0;k<=g;++k) if (equal(out->groups[k].identity.boot,id.boot) && equal(out->groups[k].identity.capture,id.capture)) {
            if (out->groups[k].identity.file_no!=cursor) reachable=false;
            if (cursor!=UINT64_MAX) ++cursor;
        }
        const pgwt_codec_stop *cut=NULL;
        for (size_t t=0;t<out->stop_count;++t) {
            const pgwt_codec_stop *stop=&out->stops[t];pgwt_codec_identity f=out->groups[stop->group].identity;
            if (!equal(f.boot,id.boot) || !equal(f.capture,id.capture)) continue;
            cap->failed=true;
            if (!cut || f.file_no<out->groups[cut->group].identity.file_no ||
                (f.file_no==out->groups[cut->group].identity.file_no && point_order(stop,cut)<0)) cut=stop;
        }
        if (cut && (out->groups[cut->group].identity.file_no<id.file_no ||
            (out->groups[cut->group].identity.file_no==id.file_no && !cut->block_offset && !cut->has_record_offset))) reachable=false;
        group->admitted=reachable;if (!reachable) continue;
        size_t src=group->sources[0],f=out->file_count++;
        group->file_index=f;out->files[f]=states[src].file;out->info[f]=states[src].info;
        memset(&states[src].file,0,sizeof(states[src].file));memset(&states[src].info,0,sizeof(states[src].info));
        pgwt_evidence_file *file=&out->files[f];pgwt_codec_file_info *info=&out->info[f];size_t retained=0;
        for (size_t r=0;r<file->record_count;++r) {
            pgwt_codec_position pos=info->positions[r];
            pgwt_codec_stop here={.block_offset=pos.block_offset,.record_offset=pos.record_offset,.has_record_offset=true};
            if (cut && out->groups[cut->group].identity.file_no==id.file_no && point_order(&here,cut)>=0) continue;
            if (file->records[r].unsupported_shape) continue;
            ((pgwt_evidence_record *)file->records)[retained]=file->records[r];
            info->positions[retained]=pos;
            ((pgwt_codec_transition *)info->descriptor.transitions)[retained]=info->descriptor.transitions[r];++retained;
        }
        file->record_count=retained;
    }
    if (capture_replay(out)) goto error;
    out->failed=out->unassigned_header_failure;
    for (size_t c=0;c<out->capture_count;++c) out->failed|=out->captures[c].failed;
    for (size_t i=0;i<n;++i) file_free(&states[i].file,&states[i].info);
    free(states);
    for (size_t i=0;i<candidate_count;++i) free(candidates[i].witnesses);
    free(candidates);return out->failed ? 1 : 0;
error:
    if (states) for (size_t i=0;i<out->delivery_count;++i) file_free(&states[i].file,&states[i].info);
    free(states);
    for (size_t i=0;i<candidate_count;++i) free(candidates[i].witnesses);
    free(candidates);pgwt_codec_decoded_free(out);return -1;
}
const char *pgwt_codec_code_name(pgwt_codec_code code)
{
    static const char *const names[]={"HC","BC","SC","PU","CRC","T","IH","IB","RF"};
    return (unsigned)code<sizeof(names)/sizeof(*names) ? names[code] : NULL;
}


static bool valid_record(const pgwt_evidence_record *r, const pgwt_codec_descriptor *d)
{
    if (r->unsupported_shape) return false;
    bool backend=r->kind==PGWT_EVIDENCE_LIFE || r->kind==PGWT_EVIDENCE_ATTACH_OK || r->kind==PGWT_EVIDENCE_END;
    if (backend && (!r->pid || (r->has_fork && r->fork>r->t))) return false;
    uint32_t a,b;
    switch (r->kind) {
    case PGWT_EVIDENCE_LIFE:
        return r->start<=r->t && (r->start_known ? (r->has_fork && r->start==r->fork) : !r->has_fork) &&
            (!r->has_exit || (r->start<=r->exit && r->exit<=r->t));
    case PGWT_EVIDENCE_ATTACH_OK:
        return r->counters.known && event_id(d,r->seed,false,&a) && event_id(d,r->state,false,&b);
    case PGWT_EVIDENCE_FRONTIER: return r->counters.known;
    case PGWT_EVIDENCE_END: return literal(r->end_reason,"detach") || literal(r->end_reason,"deescalate") || literal(r->end_reason,"stop");
    case PGWT_EVIDENCE_VOID: return valid_string(r->void_reason,4096);
    case PGWT_EVIDENCE_TRANS:
        return r->pid && r->duration<=r->t && event_id(d,r->old_event,false,&a) && event_id(d,r->new_event,true,&b) && a!=b;
    default: return false;
    }
}
static void body_write(writer *w, const pgwt_evidence_record *r, const pgwt_codec_descriptor *d)
{
    put(w,r->t,8);
    if (r->kind==PGWT_EVIDENCE_LIFE || r->kind==PGWT_EVIDENCE_ATTACH_OK || r->kind==PGWT_EVIDENCE_END) {
        put(w,r->pid,4);optional_write(w,r->has_fork,r->fork);
    }
    switch (r->kind) {
    case PGWT_EVIDENCE_LIFE: put(w,r->start,8);put(w,r->start_known,1);optional_write(w,r->has_exit,r->exit);break;
    case PGWT_EVIDENCE_ATTACH_OK: {
        uint32_t a=0,b=0;(void)event_id(d,r->seed,false,&a);(void)event_id(d,r->state,false,&b);
        put(w,r->reread,1);put(w,a,4);put(w,b,4);counters_write(w,&r->counters);break;
    }
    case PGWT_EVIDENCE_FRONTIER: counters_write(w,&r->counters);break;
    case PGWT_EVIDENCE_END: put(w,literal(r->end_reason,"detach") ? 1 : literal(r->end_reason,"deescalate") ? 2 : 3,1);break;
    case PGWT_EVIDENCE_VOID: counters_write(w,&r->counters);string_write(w,r->void_reason);optional_write(w,r->has_invalid_from,r->invalid_from);break;
    default: w->bad=true;break;
    }
}
void pgwt_codec_bytes_free(pgwt_codec_bytes *b)
{ if (b) { free(b->data);memset(b,0,sizeof(*b)); } }
int pgwt_codec_encode(const pgwt_evidence_file *f, const pgwt_codec_descriptor *d,
                      const pgwt_codec_buffer *previous, pgwt_codec_bytes *out, const char **reason)
{
    if (reason) *reason=NULL;
    if (!out) return -1;
    memset(out,0,sizeof(*out));
    if (!f || !d || !f->present || f->unsupported_shape || f->version!=5 ||
        !valid_string(f->boot,255) || !valid_string(f->capture,255) || d->mapping_count>65536 ||
        (d->mapping_count && !d->mapping) || (f->record_count && !f->records) ||
        f->record_count>SIZE_MAX/sizeof(*f->records) || d->block_records>RECORD_MAX ||
        (f->file_no==0 ? (f->has_previous || previous) : (!f->has_previous || f->previous!=f->file_no-1 || !previous))) {
        if (reason) { *reason="invalid descriptor"; } return -1;
    }
    for (size_t i=0;i<d->mapping_count;++i) {
        const pgwt_codec_mapping *m=&d->mapping[i];
        if (!m->id || m->id==UINT32_MAX || (i && m->id<=d->mapping[i-1].id) ||
            (m->role!=1 && m->role!=2) || !valid_string(m->name,1024) || literal(m->name,"$EXIT")) {
            if (reason) { *reason="invalid mapping"; } return -1;
        }
        for (size_t j=0;j<i;++j) if (equal(m->name,d->mapping[j].name)) { if (reason) { *reason="duplicate mapping name"; } return -1; }
    }
    for (size_t i=0;i<f->record_count;++i) if (!valid_record(&f->records[i],d)) { if (reason) { *reason="malformed record"; } return -1; }
    pgwt_evidence_file pf={0};pgwt_codec_file_info pi={0};size_t previous_end=0;unsigned char digest[32]={0};
    if (previous) {
        pgwt_codec_reason h=header(*previous,&pf,&pi);pgwt_codec_stop stop={0};
        int status=h==PGWT_CODEC_RECORD ? scan(&pf,&pi,&stop) : -1;
        bool ok=status==0 && pf.file_no==f->previous && equal(pf.boot,f->boot) && equal(pf.capture,f->capture) &&
            pf.qualified==f->qualified && same_mapping(&pi.descriptor,d);
        previous_end=pi.data_end;
        if (ok) predecessor_hash(*previous,previous_end,digest);
        file_free(&pf,&pi);
        if (!ok) { if (reason) { *reason="invalid predecessor"; } return -1; }
    }
    writer w={.max=HEADER_MAX}, p={.max=PAYLOAD_MAX};index_entry *index=NULL;size_t blocks=0;
    put(&w,0x54574750,4);put(&w,5,2);put(&w,d->flags,2);put(&w,d->pg_version,4);
    put(&w,d->start_time_ns,8);put(&w,d->clock_offset_ns,8);put(&w,d->nominal_sample_period_ns,8);
    write_bytes(&w,"EVD1",4);put(&w,0,4);put(&w,1,2);put(&w,0,2);
    string_write(&w,f->boot);string_write(&w,f->capture);put(&w,f->file_no,8);put(&w,f->has_previous,1);
    if (f->has_previous) { put(&w,f->previous,8);put(&w,previous_end,8);write_bytes(&w,digest,32); }
    put(&w,f->qualified,1);counters_write(&w,&f->baseline);put(&w,d->mapping_count,4);
    for (size_t i=0;i<d->mapping_count;++i) { put(&w,d->mapping[i].id,4);put(&w,d->mapping[i].role,1);string_write(&w,d->mapping[i].name); }
    if (w.bad || w.n>HEADER_MAX-4) goto fail;
    put_at(w.p+40,w.n+4,4);
    uint32_t preceding=crc_update(crc_update(UINT32_MAX,w.p,6),w.p+8,w.n-8)^UINT32_MAX;
    put(&w,preceding,4);w.max=SIZE_MAX;
    size_t limit=d->block_records ? d->block_records : RECORD_MAX;
    for (size_t i=0;i<f->record_count;) {
        bool trans=f->records[i].kind==PGWT_EVIDENCE_TRANS;size_t n=0;p.n=0;p.bad=false;
        if (!trans) { put(&p,1,2);put(&p,0,2);put(&p,0,4); }
        while (i+n<f->record_count && n<limit && (f->records[i+n].kind==PGWT_EVIDENCE_TRANS)==trans) {
            if (!trans) {
                writer body={.max=65535};body_write(&body,&f->records[i+n],d);
                if (body.bad) { free(body.p);goto fail; }
                if (body.n+5>PAYLOAD_MAX-p.n) { free(body.p);break; }
                static const unsigned wire[]={0,1,2,0,3,4,5};
                put(&p,wire[f->records[i+n].kind],1);put(&p,body.n,4);write_bytes(&p,body.p,body.n);free(body.p);
            }
            ++n;
        }
        if (trans) {
            uint64_t prev=0;
            for (size_t j=0;j<n;++j) { uleb_write(&p,f->records[i+j].t-prev);prev=f->records[i+j].t; }
            for (size_t j=0;j<n;++j) put(&p,f->records[i+j].pid,4);
            for (size_t j=0;j<n;++j) { uint32_t id=0;(void)event_id(d,f->records[i+j].old_event,false,&id);put(&p,id,4); }
            for (size_t j=0;j<n;++j) { uint32_t id=0;(void)event_id(d,f->records[i+j].new_event,true,&id);put(&p,id,4); }
            for (size_t j=0;j<n;++j) uleb_write(&p,f->records[i+j].duration);
            for (size_t j=0;j<n;++j) put(&p,d->transitions ? d->transitions[i+j].query_id : 0,8);
            for (size_t j=0;j<n;++j) uleb_write(&p,d->transitions ? d->transitions[i+j].cpu_ns : 0);
        } else put_at(p.p+4,n,4);
        if (p.bad || !n || blocks>=BLOCK_MAX) goto fail;
        index_entry *grown=array_grow(index,blocks+1,sizeof(*index));
        if (!grown) goto fail;
        index=grown;
        size_t offset=w.n;uint64_t first=trans ? f->records[i].t : 0,last=trans ? f->records[i+n-1].t : 0;
        index[blocks++]=(index_entry){first,offset};
        put(&w,first,8);put(&w,last,8);put(&w,n,4);put(&w,p.n,4);put(&w,p.n,4);put(&w,trans?0:2,2);put(&w,0,2);
        put(&w,0,8);put(&w,0,8);put(&w,offset,8);put(&w,preceding,4);put(&w,0,4);write_bytes(&w,p.p,p.n);
        if (w.bad) goto fail;
        preceding=pgwt_codec_crc32(w.p+offset,w.n-offset);put(&w,preceding,4);i+=n;
    }
    if (d->footer) {
        for (size_t i=0;i<blocks;++i) { put(&w,index[i].time,8);put(&w,index[i].offset,8); }put(&w,blocks,4);
    }
    if (w.bad) goto fail;
    free(p.p);free(index);out->data=w.p;out->size=w.n;return 0;
fail:
    free(w.p);free(p.p);free(index);if (reason) { *reason="resource limit or allocation failure"; } return -1;
}
const char *pgwt_codec_reason_name(pgwt_codec_reason r)
{
    static const char *const names[]={"none","header","schema","bounds","framing","crc","link","encoding","record"};
    return (unsigned)r<sizeof(names)/sizeof(*names) ? names[r] : "invalid";
}
