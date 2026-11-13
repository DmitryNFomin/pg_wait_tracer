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
/* Scan one file; record faults are retained with their precise positions for
 * r3 replay. They do not turn any decoded suffix into certified evidence. */
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
            else if (s>remaining-64 || remaining-64-s<4) reason=PGWT_CODEC_FRAMING;
            else if (pgwt_codec_crc32(bh,64+s)!=at(bh+64+s,4)) reason=PGWT_CODEC_CRC;
            else if (at(bh+48,8)!=off || at(bh+56,4)!=preceding) reason=PGWT_CODEC_LINK;
            else if (type>2 || codec>1 || (type==2 && codec) || at(bh+30,2) || bh[61] || bh[62] || bh[63]) reason=PGWT_CODEC_SCHEMA;
            else if ((codec==0 && s!=u) || (type!=1 && (at(bh+32,8) || at(bh+40,8)))) reason=PGWT_CODEC_ENCODING;
        }
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
            *stop=(pgwt_codec_stop){stop->input_file,off,0,info->data_end,reason};
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
        if (fault) break;
    }
    free(index);free(rows);
    if (fault) { *stop=(pgwt_codec_stop){stop->input_file,fault_block,fault_record,info->data_end,fault};return 1; }
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
    free(out->files);free(out->info);free(out->stops);memset(out,0,sizeof(*out));
}
static bool same_mapping(const pgwt_codec_descriptor *a, const pgwt_codec_descriptor *b)
{
    if (a->mapping_count!=b->mapping_count) return false;
    for (size_t i=0;i<a->mapping_count;++i)
        if (a->mapping[i].id!=b->mapping[i].id || a->mapping[i].role!=b->mapping[i].role || !equal(a->mapping[i].name,b->mapping[i].name)) return false;
    return true;
}
int pgwt_codec_decode(const pgwt_codec_buffer *buffers, size_t count, pgwt_codec_decoded *out)
{
    if (!out) return -1;
    memset(out,0,sizeof(*out));
    if ((count && !buffers) || count>SIZE_MAX/sizeof(*out->files) || count>SIZE_MAX/sizeof(*out->info) || count>SIZE_MAX/sizeof(*out->stops)) return -1;
    if (!count) return 0;
    out->files=calloc(count,sizeof(*out->files));out->info=calloc(count,sizeof(*out->info));out->stops=calloc(count,sizeof(*out->stops));
    bool *stopped=calloc(count,sizeof(*stopped));
    if (!out->files || !out->info || !out->stops || !stopped) goto error;
    for (size_t i=0;i<count;++i) {
        size_t j=out->file_count;pgwt_evidence_file *f=&out->files[j];pgwt_codec_file_info *info=&out->info[j];
        pgwt_codec_stop stop={.input_file=i};
        pgwt_codec_reason hr=header(buffers[i],f,info);
        if (hr==PGWT_CODEC_NONE) { file_free(f,info);goto error; }
        if (hr!=PGWT_CODEC_RECORD) {
            file_free(f,info);memset(f,0,sizeof(*f));memset(info,0,sizeof(*info));
            stop.reason=hr;out->stops[out->stop_count++]=stop;continue;
        }
        size_t prev=SIZE_MAX;
        for (size_t k=0;k<j;++k) if (equal(out->files[k].capture,f->capture)) prev=k;
        if (prev!=SIZE_MAX && stopped[prev]) { file_free(f,info);memset(f,0,sizeof(*f));memset(info,0,sizeof(*info));continue; }
        bool link_bad=prev==SIZE_MAX ? f->file_no!=0 :
            (!equal(out->files[prev].boot,f->boot) || out->files[prev].file_no==UINT64_MAX || f->file_no!=out->files[prev].file_no+1 ||
             out->files[prev].qualified!=f->qualified || !same_mapping(&out->info[prev].descriptor,&info->descriptor) ||
             info->previous_data_end!=out->info[prev].data_end);
        if (!link_bad && prev!=SIZE_MAX) {
            unsigned char digest[32];predecessor_hash(out->info[prev].bytes,out->info[prev].data_end,digest);
            link_bad=memcmp(digest,info->previous_sha256,32)!=0;
        }
        if (link_bad) {
            stop.reason=PGWT_CODEC_LINK;out->stops[out->stop_count++]=stop;
            if (prev!=SIZE_MAX) stopped[prev]=true;
            /* Keep a rejected, identified descriptor as a tombstone for this
             * capture; replay stops before it, including when it is the root. */
            f->unsupported_shape=true;stopped[j]=true;++out->file_count;continue;
        }
        if (prev!=SIZE_MAX) {
            info->has_samples=out->info[prev].has_samples;
            info->last_sample_time=out->info[prev].last_sample_time;
            info->last_sample_weight=out->info[prev].last_sample_weight;
        }
        int status=scan(f,info,&stop);
        if (status<0) { ++out->file_count;goto error; }
        if (status) { out->stops[out->stop_count++]=stop;stopped[j]=true; }
        ++out->file_count;
    }
    free(stopped);return out->stop_count ? 1 : 0;
error:
    free(stopped);pgwt_codec_decoded_free(out);return -1;
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
