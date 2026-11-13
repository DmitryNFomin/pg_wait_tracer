#define _POSIX_C_SOURCE 200809L
#include "core/evidence_codec.h"
#include "evidence_json.h"
#include <glob.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int failures;
static const char *only;
static bool selected(const char *name) { return !only || !strcmp(only,name); }
static void check(const char *name, bool ok)
{
    if (!selected(name)) return;
    printf("codec %s %s\n",name,ok ? "PASS" : "FAIL assertion");
    if (!ok) ++failures;
}
static uint64_t rd(const unsigned char *p, unsigned n)
{ uint64_t v=0;for (unsigned i=0;i<n;++i) v|=(uint64_t)p[i]<<(8*i);return v; }
static void wr(unsigned char *p, uint64_t v, unsigned n)
{ for (unsigned i=0;i<n;++i) { p[i]=(unsigned char)v;v>>=8; } }
static pgwt_codec_buffer view(pgwt_codec_bytes b) { return (pgwt_codec_buffer){b.data,b.size}; }
static pgwt_codec_bytes copy(pgwt_codec_buffer b)
{ pgwt_codec_bytes c={malloc(b.size+1),b.size};if (!c.data) { exit(2); } memcpy(c.data,b.data,b.size);return c; }
static pgwt_codec_bytes load(const char *path)
{
    FILE *f=fopen(path,"rb");if (!f) { perror(path);exit(2); }
    if (fseek(f,0,SEEK_END)) exit(2);
    long size=ftell(f);if (size<0 || fseek(f,0,SEEK_SET)) exit(2);
    pgwt_codec_bytes b={malloc((size_t)size+1),(size_t)size};
    if (!b.data || fread(b.data,1,b.size,f)!=b.size || fclose(f)) exit(2);
    return b;
}
static pgwt_codec_bytes golden(const char *root)
{
    pgwt_codec_bytes all={malloc(646),0};if (!all.data) exit(2);
    for (int i=1;i<=3;++i) {
        char path[4096];snprintf(path,sizeof(path),"%s/codec/fixtures/ex%d.bin",root,i);
        pgwt_codec_bytes b=load(path);
        if (b.size>646-all.size) exit(2);
        memcpy(all.data+all.size,b.data,b.size);all.size+=b.size;pgwt_codec_bytes_free(&b);
    }
    if (all.size!=646) exit(2);
    return all;
}
static void header_crc(pgwt_codec_bytes b)
{
    size_t h=(size_t)rd(b.data+40,4);unsigned char *p=malloc(h-6);if (!p) exit(2);
    memcpy(p,b.data,6);memcpy(p+6,b.data+8,h-12);
    wr(b.data+h-4,pgwt_codec_crc32(p,h-6),4);free(p);
}
/* Test-only integrity repair. It never conceals a mutation from the decoder:
 * each framing/body test recomputes CRCs so its intended check is reached. */
static void repair(pgwt_codec_bytes b)
{
    header_crc(b);size_t off=(size_t)rd(b.data+40,4);uint32_t prev=(uint32_t)rd(b.data+off-4,4);
    while (off<=b.size && b.size-off>=68) {
        size_t s=(size_t)rd(b.data+off+20,4);
        if (!s || !rd(b.data+off+16,4) || s>b.size-off-68) break;
        wr(b.data+off+56,prev,4);
        prev=pgwt_codec_crc32(b.data+off,64+s);wr(b.data+off+64+s,prev,4);off+=68+s;
    }
}
static bool stop_is(pgwt_codec_buffer b, pgwt_codec_reason why, size_t block, size_t end, size_t records, size_t rec)
{
    pgwt_codec_decoded out;int status=pgwt_codec_decode(&b,1,&out);
    bool ok=status==1 && out.stop_count==1 && out.stops[0].input_file==0 &&
        out.stops[0].reason==why && out.stops[0].block_offset==block &&
        out.stops[0].validated_end==end && out.stops[0].record_offset==rec &&
        (out.file_count ? out.files[0].record_count : 0)==records;
    if (!ok && !only) fprintf(stderr,"stop expected %s block=%zu end=%zu records=%zu rec=%zu; status=%d files=%zu stops=%zu actual=%s/%zu/%zu/%zu/%zu\n",
        pgwt_codec_reason_name(why),block,end,records,rec,status,out.file_count,out.stop_count,
        out.stop_count ? pgwt_codec_reason_name(out.stops[0].reason) : "none",
        out.stop_count ? out.stops[0].block_offset : 0,out.stop_count ? out.stops[0].validated_end : 0,
        out.file_count ? out.files[0].record_count : 0,out.stop_count ? out.stops[0].record_offset : 0);
    pgwt_codec_decoded_free(&out);return ok;
}
static void mutate(pgwt_codec_buffer seed, const char *name, size_t pos, uint64_t v, unsigned n,
                   bool fix, pgwt_codec_reason why, size_t block, size_t end, size_t records, size_t rec)
{
    if (!selected(name)) return;
    if (pos>seed.size || n>seed.size-pos) { fprintf(stderr,"bad test mutation %s\n",name);exit(2); }
    pgwt_codec_bytes b=copy(seed);wr(b.data+pos,v,n);if (fix) repair(b);
    check(name,stop_is(view(b),why,block,end,records,rec));pgwt_codec_bytes_free(&b);
}
static void digest_hex(const unsigned char *digest, char out[65])
{ for (size_t i=0;i<32;++i) snprintf(out+2*i,3,"%02x",digest[i]); }
static void known_answers(pgwt_codec_buffer g)
{
    check("crc_known_answers",pgwt_codec_crc32("123456789",9)==0xcbf43926 && pgwt_codec_crc32("\0",1)==0xd202ef8d && pgwt_codec_crc32("",0)==0);
    unsigned char digest[32];char hex[65];
    pgwt_codec_sha256("",0,digest);digest_hex(digest,hex);
    bool ok=!strcmp(hex,"e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    pgwt_codec_sha256("abc",3,digest);digest_hex(digest,hex);
    ok &= !strcmp(hex,"ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    const char *multi="abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    pgwt_codec_sha256(multi,strlen(multi),digest);digest_hex(digest,hex);
    ok &= !strcmp(hex,"248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    unsigned char *million=malloc(1000000);if (!million) { exit(2); } memset(million,'a',1000000);
    pgwt_codec_sha256(million,1000000,digest);digest_hex(digest,hex);free(million);
    ok &= !strcmp(hex,"cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
    check("sha256_known_answers",ok);
    unsigned char joined[644];memcpy(joined,g.data,6);memcpy(joined+6,g.data+8,638);
    pgwt_codec_sha256(joined,sizeof(joined),digest);digest_hex(digest,hex);
    check("golden_digest",!strcmp(hex,"4c24de25c90c09fe8f9f33b25899eeaa0223eb4d348176c38ae3c2c28edd5463"));
}
static pgwt_codec_bytes golden_roundtrip(pgwt_codec_buffer g)
{
    pgwt_codec_decoded out;int status=pgwt_codec_decode(&g,1,&out);pgwt_codec_bytes encoded={0};
    bool ok=status==0 && out.file_count==1 && out.files[0].record_count==4 && out.info[0].data_end==646 &&
        out.info[0].block_count==3 && out.info[0].positions[0].block_offset==201 && out.info[0].positions[0].record_offset==8 &&
        out.info[0].positions[1].record_offset==36 && out.info[0].positions[2].block_offset==453 && out.info[0].positions[2].record_offset==0 &&
        out.info[0].positions[3].block_offset==544 && out.info[0].positions[3].record_offset==8;
    if (ok) ok=pgwt_codec_encode(&out.files[0],&out.info[0].descriptor,NULL,&encoded,NULL)==0 &&
        encoded.size==g.size && !memcmp(encoded.data,g.data,g.size);
    check("golden_646_bytes",ok);pgwt_codec_decoded_free(&out);return encoded;
}

static void refusal_rows(pgwt_codec_buffer g)
{
    /* §9 rows 1–6: no record of a rejected validation unit may escape. */
    mutate(g,"row1_file_magic",0,0,4,true,PGWT_CODEC_HEADER,0,0,0,0);
    mutate(g,"row1_trace_version",4,4,2,true,PGWT_CODEC_HEADER,0,0,0,0);
    mutate(g,"row1_descriptor_version",44,2,2,true,PGWT_CODEC_HEADER,0,0,0,0);
    mutate(g,"row1_extension_magic",36,0,1,true,PGWT_CODEC_HEADER,0,0,0,0);
    mutate(g,"row1_header_reserved",46,1,2,true,PGWT_CODEC_HEADER,0,0,0,0);
    mutate(g,"row1_header_length_small",40,72,4,false,PGWT_CODEC_HEADER,0,0,0,0);
    mutate(g,"row1_header_length_large",40,16777217,4,false,PGWT_CODEC_HEADER,0,0,0,0);
    mutate(g,"row1_empty_boot",48,0,2,true,PGWT_CODEC_HEADER,0,0,0,0);
    mutate(g,"row1_bad_qualified",63,2,1,true,PGWT_CODEC_HEADER,0,0,0,0);
    mutate(g,"row1_mapping_zero",189,0,4,true,PGWT_CODEC_HEADER,0,0,0,0);
    mutate(g,"row1_mapping_exit_id",189,UINT32_MAX,4,true,PGWT_CODEC_HEADER,0,0,0,0);
    mutate(g,"row1_mapping_role",193,3,1,true,PGWT_CODEC_HEADER,0,0,0,0);
    mutate(g,"row1_mapping_name_empty",194,0,2,true,PGWT_CODEC_HEADER,0,0,0,0);
    mutate(g,"row1_mapping_count_limit",185,65537,4,true,PGWT_CODEC_HEADER,0,0,0,0);
    mutate(g,"row1_bad_previous",62,1,1,true,PGWT_CODEC_HEADER,0,0,0,0);
    mutate(g,"row2_payload_version",265,2,2,true,PGWT_CODEC_SCHEMA,201,201,0,0);
    mutate(g,"row2_payload_reserved",267,1,2,true,PGWT_CODEC_SCHEMA,201,201,0,0);
    mutate(g,"row2_block_type",229,3,2,true,PGWT_CODEC_SCHEMA,201,201,0,0);
    mutate(g,"row2_evidence_unknown_codec",261,2,1,true,PGWT_CODEC_SCHEMA,201,201,0,0);
    mutate(g,"row2_evidence_compressed",261,1,1,true,PGWT_CODEC_SCHEMA,201,201,0,0);
    mutate(g,"row2_block_reserved",231,1,2,true,PGWT_CODEC_SCHEMA,201,201,0,0);
    mutate(g,"row2_codec_reserved",262,1,1,true,PGWT_CODEC_SCHEMA,201,201,0,0);
    mutate(g,"row3_count_zero",217,0,4,true,PGWT_CODEC_BOUNDS,201,201,0,0);
    mutate(g,"row3_count_4097",217,4097,4,true,PGWT_CODEC_BOUNDS,201,201,0,0);
    mutate(g,"row3_unequal_counts",269,1,4,true,PGWT_CODEC_BOUNDS,201,201,0,0);
    mutate(g,"row3_stored_zero",221,0,4,true,PGWT_CODEC_BOUNDS,201,201,0,0);
    mutate(g,"row3_stored_oversize",221,10485761,4,false,PGWT_CODEC_BOUNDS,201,201,0,0);
    mutate(g,"row3_decoded_zero",225,0,4,true,PGWT_CODEC_BOUNDS,201,201,0,0);
    mutate(g,"row3_decoded_oversize",225,10485761,4,true,PGWT_CODEC_BOUNDS,201,201,0,0);
    mutate(g,"row3_missing_common_t",274,7,4,true,PGWT_CODEC_BOUNDS,201,201,0,0);
    mutate(g,"row3_body_oversize",274,65536,4,true,PGWT_CODEC_BOUNDS,201,201,0,0);
    mutate(g,"row4_body_overrun",302,65535,4,true,PGWT_CODEC_FRAMING,201,201,0,0);
    mutate(g,"row4_suffix",269,1,4,false,PGWT_CODEC_CRC,201,201,0,0);
    /* One intact frame plus a suffix (shrink count and num_events together). */
    if (selected("row4_unconsumed_suffix")) {
        pgwt_codec_bytes b=copy(g);wr(b.data+217,1,4);wr(b.data+269,1,4);repair(b);
        check("row4_unconsumed_suffix",stop_is(view(b),PGWT_CODEC_FRAMING,201,201,0,0));pgwt_codec_bytes_free(&b);
    }
    for (size_t cut=0;cut<3;++cut) {
        const char *name=cut==0 ? "row4_truncated_header" : cut==1 ? "row4_truncated_payload" : "row4_truncated_crc";
        pgwt_codec_buffer b=g;b.size=cut==0 ? 240 : cut==1 ? 310 : 451;
        check(name,stop_is(b,PGWT_CODEC_FRAMING,201,201,0,0));
    }
    mutate(g,"row5_header_crc",197,0,4,false,PGWT_CODEC_CRC,0,0,0,0);
    mutate(g,"row5_block_crc",449,0,4,false,PGWT_CODEC_CRC,201,201,0,0);
    mutate(g,"row5_self_offset",249,202,8,true,PGWT_CODEC_LINK,201,201,0,0);
    /* repair() would repair this field, so update only this block's checksum. */
    if (selected("row5_preceding_crc")) {
        pgwt_codec_bytes b=copy(g);wr(b.data+257,7,4);wr(b.data+449,pgwt_codec_crc32(b.data+201,248),4);
        check("row5_preceding_crc",stop_is(view(b),PGWT_CODEC_LINK,201,201,0,0));pgwt_codec_bytes_free(&b);
    }
    mutate(g,"row5_second_block_atomic",540,0,4,false,PGWT_CODEC_CRC,453,453,2,0);
    mutate(g,"row6_raw_size",225,185,4,true,PGWT_CODEC_ENCODING,201,201,0,0);
    mutate(g,"row6_nonminimal_uleb",517,0x80,1,true,PGWT_CODEC_ENCODING,453,453,2,0);
    mutate(g,"row6_type0_time",453,6,8,true,PGWT_CODEC_ENCODING,453,453,2,0);
    mutate(g,"row6_type0_weights",485,1,8,true,PGWT_CODEC_ENCODING,453,453,2,0);
    mutate(g,"row6_type2_weights",241,1,8,true,PGWT_CODEC_ENCODING,201,201,0,0);
    /* §9 rows 7–9: intact framing exposes an ordered unsupported_shape, r3
     * stops at that position. The other record of this block remains valid. */
    mutate(g,"row7_unknown_kind",301,99,1,true,PGWT_CODEC_RECORD,201,453,2,36);
    mutate(g,"row7_unknown_method",319,2,1,true,PGWT_CODEC_RECORD,201,453,2,36);
    mutate(g,"row8_bad_optional",290,2,1,true,PGWT_CODEC_RECORD,201,453,2,8);
    mutate(g,"row8_bad_pid",286,0,4,true,PGWT_CODEC_RECORD,201,453,2,8);
    mutate(g,"row8_bad_start_known",299,2,1,true,PGWT_CODEC_RECORD,201,453,2,8);
    mutate(g,"row8_bad_life_start",291,2,8,true,PGWT_CODEC_RECORD,201,453,2,8);
    mutate(g,"row8_bad_void_reason",630,0,2,true,PGWT_CODEC_RECORD,544,646,4,8);
    mutate(g,"row9_unresolved_event",324,99,4,true,PGWT_CODEC_RECORD,201,453,2,36);
    mutate(g,"row9_duration_gt_t",530,6,1,true,PGWT_CODEC_RECORD,453,544,3,0);
    mutate(g,"row9_same_event",522,0,4,true,PGWT_CODEC_RECORD,453,544,3,0);
    mutate(g,"row9_unmapped_old",522,99,4,true,PGWT_CODEC_RECORD,453,544,3,0);
    /* Hints and flags cannot change validation or replay. */
    if (selected("type2_hints_flags")) {
        pgwt_codec_bytes b=copy(g);wr(b.data+6,65535,2);wr(b.data+201,UINT64_MAX,8);wr(b.data+209,1,8);repair(b);
        pgwt_codec_buffer v=view(b);pgwt_codec_decoded out;int status=pgwt_codec_decode(&v,1,&out);
        check("type2_hints_flags",status==0 && out.files[0].record_count==4 && out.files[0].records[0].t==1);
        pgwt_codec_decoded_free(&out);pgwt_codec_bytes_free(&b);
    }
}

static void predecessor_tests(pgwt_codec_buffer g)
{
    pgwt_codec_decoded a;pgwt_codec_bytes next={0};
    if (pgwt_codec_decode(&g,1,&a)) exit(2);
    pgwt_evidence_file f=a.files[0];f.file_no=1;f.has_previous=true;f.previous=0;f.record_count=0;
    pgwt_codec_descriptor d=a.info[0].descriptor;d.footer=true;
    if (pgwt_codec_encode(&f,&d,&g,&next,NULL)) exit(2);
    pgwt_codec_buffer chain[]={g,view(next)};pgwt_codec_decoded out;
    check("predecessor_valid",pgwt_codec_decode(chain,2,&out)==0 && out.file_count==2 && out.info[1].descriptor.footer);
    pgwt_codec_decoded_free(&out);
    const char *names[]={"row5_predecessor_digest","row5_predecessor_end","row5_predecessor_ordinal"};
    size_t offsets[]={79,71,63};
    for (size_t i=0;i<3;++i) {
        pgwt_codec_bytes b=copy(view(next));b.data[offsets[i]]^=1;header_crc(b);chain[1]=view(b);
        int status=pgwt_codec_decode(chain,2,&out);
        check(names[i],status==1 && out.stop_count==1 && out.stops[0].input_file==1 &&
            out.stops[0].block_offset==0 && out.stops[0].validated_end==0 &&
            out.stops[0].reason==(i==2 ? PGWT_CODEC_HEADER : PGWT_CODEC_LINK) && out.files[0].record_count==4 &&
            (out.file_count<2 || !out.files[1].record_count));
        pgwt_codec_decoded_free(&out);pgwt_codec_bytes_free(&b);
    }
    pgwt_codec_bytes flags=copy(g);flags.data[6]=255;flags.data[7]=255;
    chain[0]=view(flags);chain[1]=view(next);
    check("predecessor_mutable_flags",pgwt_codec_decode(chain,2,&out)==0 && out.file_count==2);
    pgwt_codec_decoded_free(&out);pgwt_codec_bytes_free(&flags);
    d=a.info[0].descriptor;d.footer=true;pgwt_codec_bytes with_footer={0};
    if (pgwt_codec_encode(&a.files[0],&d,NULL,&with_footer,NULL)) exit(2);
    chain[0]=view(with_footer);
    check("footer_digest_excluded",pgwt_codec_decode(chain,2,&out)==0 && out.file_count==2 && out.info[0].data_end==646 && out.info[0].descriptor.footer);
    pgwt_codec_decoded_free(&out);
    with_footer.data[with_footer.size-4]^=1;
    check("footer_bad_tail",stop_is(view(with_footer),PGWT_CODEC_FRAMING,646,646,4,0));
    pgwt_codec_bytes_free(&with_footer);pgwt_codec_bytes_free(&next);pgwt_codec_decoded_free(&a);
}

/* Construct one block using the independently frozen header. Tests specify
 * payload bytes rather than calling the product payload encoder. */
static pgwt_codec_bytes block_bytes(pgwt_codec_buffer g, unsigned type, size_t count,
                                    const unsigned char *p, size_t s, size_t u,
                                    unsigned codec, uint64_t first, uint64_t last,
                                    uint64_t period, uint64_t maximum)
{
    pgwt_codec_bytes b={calloc(201+68+s,1),201+68+s};if (!b.data) exit(2);
    memcpy(b.data,g.data,201);unsigned char *h=b.data+201;
    wr(h,first,8);wr(h+8,last,8);wr(h+16,count,4);wr(h+20,s,4);wr(h+24,u,4);
    wr(h+28,type,2);wr(h+32,period,8);wr(h+40,maximum,8);wr(h+48,201,8);h[60]=(unsigned char)codec;
    memcpy(h+64,p,s);repair(b);return b;
}
static bool complete_records(pgwt_codec_buffer b, size_t count)
{
    pgwt_codec_decoded out;int status=pgwt_codec_decode(&b,1,&out);
    bool ok=status==0 && out.file_count==1 && out.files[0].record_count==count;
    pgwt_codec_decoded_free(&out);return ok;
}
static pgwt_codec_bytes join_blocks(pgwt_codec_buffer a, pgwt_codec_buffer b)
{
    size_t h=(size_t)rd(b.data+40,4);
    pgwt_codec_bytes out={malloc(a.size+b.size-h),a.size+b.size-h};if (!out.data) exit(2);
    memcpy(out.data,a.data,a.size);memcpy(out.data+a.size,b.data+h,b.size-h);
    size_t off=(size_t)rd(out.data+40,4);
    while (off<out.size) { wr(out.data+off+48,off,8);off+=68+(size_t)rd(out.data+off+20,4); }
    repair(out);return out;
}
static void sample_stream(pgwt_codec_buffer g)
{
    unsigned char p[17]={5,7,0,0,0,1,0,0,0};
    pgwt_codec_bytes first=block_bytes(g,1,1,p,17,17,0,5,5,3,3);
    pgwt_codec_bytes next=block_bytes(g,1,1,p,17,17,0,5,5,3,3);
    pgwt_codec_bytes both=join_blocks(view(first),view(next));
    check("sample_split_tick",complete_records(view(both),0));pgwt_codec_bytes_free(&both);
    wr(next.data+233,4,8);wr(next.data+241,4,8);repair(next);
    both=join_blocks(view(first),view(next));
    check("row6_sample_split_weight",stop_is(view(both),PGWT_CODEC_ENCODING,first.size,first.size,0,0));pgwt_codec_bytes_free(&both);
    /* A linked successor inherits the last sample tick even though no samples
     * are emitted to the exact logical API. */
    pgwt_codec_buffer v=view(first);pgwt_codec_decoded out;if (pgwt_codec_decode(&v,1,&out)) exit(2);
    pgwt_evidence_file f=out.files[0];f.file_no=1;f.has_previous=true;f.previous=0;
    pgwt_codec_descriptor d=out.info[0].descriptor;pgwt_codec_bytes successor={0};
    if (pgwt_codec_encode(&f,&d,&v,&successor,NULL)) exit(2);
    pgwt_codec_bytes linked=join_blocks(view(successor),view(next));
    pgwt_codec_buffer chain[]={view(first),view(linked)};pgwt_codec_decoded decoded;int status=pgwt_codec_decode(chain,2,&decoded);
    check("row6_sample_rotation_weight",status==1 && decoded.file_count==2 && decoded.stop_count==1 &&
        decoded.stops[0].input_file==1 && decoded.stops[0].reason==PGWT_CODEC_ENCODING &&
        decoded.stops[0].block_offset==successor.size && decoded.stops[0].validated_end==successor.size &&
        !decoded.files[0].record_count && !decoded.files[1].record_count);
    pgwt_codec_decoded_free(&decoded);pgwt_codec_bytes_free(&linked);pgwt_codec_bytes_free(&successor);pgwt_codec_decoded_free(&out);
    p[0]=4;pgwt_codec_bytes_free(&next);next=block_bytes(g,1,1,p,17,17,0,4,4,3,3);
    both=join_blocks(view(first),view(next));
    check("row6_sample_stream_order",stop_is(view(both),PGWT_CODEC_ENCODING,first.size,first.size,0,0));
    pgwt_codec_bytes_free(&both);pgwt_codec_bytes_free(&first);pgwt_codec_bytes_free(&next);
}
static void compression_samples(pgwt_codec_buffer g)
{
    const unsigned char *raw=g.data+517;unsigned char compressed[128];
    compressed[0]=0xf0;compressed[1]=8;memcpy(compressed+2,raw,23);
    pgwt_codec_bytes b=block_bytes(g,0,1,compressed,25,23,1,5,5,0,0);
    check("lz4_all_literals",complete_records(view(b),1));
    mutate(view(b),"row2_block_codec",261,2,1,true,PGWT_CODEC_SCHEMA,201,201,0,0);
    pgwt_codec_bytes_free(&b);
    /* Seven literals, six overlapping zero bytes, then ten final literals. */
    compressed[0]=0x72;memcpy(compressed+1,raw,7);compressed[8]=1;compressed[9]=0;
    compressed[10]=0xa0;memcpy(compressed+11,raw+13,10);
    b=block_bytes(g,0,1,compressed,21,23,1,5,5,0,0);
    check("lz4_overlapping_match",complete_records(view(b),1));pgwt_codec_bytes_free(&b);
    static const unsigned char zero_distance[]={0x10,5,0,0,0x50,0,0,0,0,0};
    static const unsigned char far_distance[]={0x10,5,2,0,0x50,0,0,0,0,0};
    static const unsigned char missing_extension[]={0xf0};
    static const unsigned char missing_last_literals[]={0x1f,5,1,0,3};
    /* Both streams decode to valid raw columns with duration=0. Each violates
     * exactly one end constraint, so neither check can hide a missing other. */
    unsigned char short_last_literals[17]={0x96};memcpy(short_last_literals+1,raw,9);
    short_last_literals[10]=1;short_last_literals[12]=0x40;
    unsigned char short_last_match[23]={0xe0};memcpy(short_last_match+1,raw,14);
    short_last_match[14]=0;short_last_match[15]=1;short_last_match[17]=0x50;
    static const unsigned char literal_overrun[]={0x50,0};
    const struct { const char *name;const unsigned char *p;size_t s,u; } bad[]={
        {"row6_lz4_zero_distance",zero_distance,sizeof(zero_distance),23},
        {"row6_lz4_far_distance",far_distance,sizeof(far_distance),23},
        {"row6_lz4_missing_extension",missing_extension,sizeof(missing_extension),23},
        {"row6_lz4_missing_final",missing_last_literals,sizeof(missing_last_literals),23},
        {"row6_lz4_last_five",short_last_literals,sizeof(short_last_literals),23},
        {"row6_lz4_last_match",short_last_match,sizeof(short_last_match),23},
        {"row6_lz4_literal_overrun",literal_overrun,sizeof(literal_overrun),23},
        {"row6_lz4_wrong_output",compressed,21,24}
    };
    for (size_t i=0;i<sizeof(bad)/sizeof(*bad);++i) {
        b=block_bytes(g,0,1,bad[i].p,bad[i].s,bad[i].u,1,5,5,0,0);
        check(bad[i].name,stop_is(view(b),PGWT_CODEC_ENCODING,201,201,0,0));pgwt_codec_bytes_free(&b);
    }
    unsigned char overflow[32];memset(overflow,255,9);overflow[9]=2;memcpy(overflow+10,raw+1,22);
    b=block_bytes(g,0,1,overflow,32,32,0,UINT64_MAX/2,UINT64_MAX/2,0,0);
    check("row6_uleb_overflow",stop_is(view(b),PGWT_CODEC_ENCODING,201,201,0,0));pgwt_codec_bytes_free(&b);
    overflow[0]=0x85;overflow[1]=0;memcpy(overflow+2,raw+1,22);
    b=block_bytes(g,0,1,overflow,24,24,0,5,5,0,0);
    check("row6_uleb_redundant",stop_is(view(b),PGWT_CODEC_ENCODING,201,201,0,0));pgwt_codec_bytes_free(&b);
    unsigned char sample[17]={5,7,0,0,0,1,0,0,0};
    b=block_bytes(g,1,1,sample,sizeof(sample),sizeof(sample),0,5,5,3,3);
    check("sample_omitted",complete_records(view(b),0));
    mutate(view(b),"row6_sample_weight",241,4,8,true,PGWT_CODEC_ENCODING,201,201,0,0);
    mutate(view(b),"row9_sample_pid",266,0,4,true,PGWT_CODEC_RECORD,201,b.size,1,0);
    mutate(view(b),"row9_sample_event",270,99,4,true,PGWT_CODEC_RECORD,201,b.size,1,0);
    pgwt_codec_bytes_free(&b);
    unsigned char marker[23];memcpy(marker,raw,23);wr(marker+1,0,4);wr(marker+9,1,4);marker[13]=0;
    b=block_bytes(g,0,1,marker,23,23,0,5,5,0,0);b.data[193]=2;repair(b);
    check("marker_pid_zero_omitted",complete_records(view(b),0));
    mutate(view(b),"row9_marker_duration",278,1,1,true,PGWT_CODEC_RECORD,201,b.size,1,0);
    pgwt_codec_bytes_free(&b);
    /* Column layout is trustworthy even when an ordinary row has pid=0. */
    b=block_bytes(g,0,1,raw,23,23,0,5,5,0,0);
    mutate(view(b),"row9_trans_pid",266,0,4,true,PGWT_CODEC_RECORD,201,b.size,1,0);
    pgwt_codec_bytes_free(&b);
}

static void record_shapes(pgwt_codec_buffer g)
{
    /* FRONTIER kind 3 is not the numeric C TRANS enum. A null vector is a
     * readable r3 loss report (§9 row 10 superseded), not unsupported_shape. */
    unsigned char p[256]={1,0,0,0,1,0,0,0,3,9,0,0,0};wr(p+13,12,8);p[21]=0;
    pgwt_codec_bytes b=block_bytes(g,2,1,p,22,22,0,0,0,0,0);
    pgwt_codec_buffer v=view(b);pgwt_codec_decoded out;int status=pgwt_codec_decode(&v,1,&out);
    pgwt_codec_bytes refused={0};const char *reason=NULL;
    bool ok=status==0 && out.files[0].record_count==1 && out.files[0].records[0].kind==PGWT_EVIDENCE_FRONTIER &&
        !out.files[0].records[0].unsupported_shape && !out.files[0].records[0].counters.known &&
        pgwt_codec_encode(&out.files[0],&out.info[0].descriptor,NULL,&refused,&reason)==-1 && !refused.data && !refused.size;
    check("row10_null_certificate_r3",ok);pgwt_codec_bytes_free(&refused);pgwt_codec_decoded_free(&out);
    mutate(v,"row8_bad_counter_bool",286,2,1,true,PGWT_CODEC_RECORD,201,b.size,1,8);
    /* Extra tail in a bounded record is a record fault, whereas an outer
     * suffix and a frame without a common t reject the whole block. */
    p[22]=99;wr(p+9,10,4);pgwt_codec_bytes tail=block_bytes(g,2,1,p,23,23,0,0,0,0,0);
    check("row8_body_tail",stop_is(view(tail),PGWT_CODEC_RECORD,201,tail.size,1,8));pgwt_codec_bytes_free(&tail);
    pgwt_codec_bytes_free(&b);
    /* END generation: t=12, pid=7, absent fork, reason=9. */
    memset(p,0,sizeof(p));p[0]=1;p[4]=1;p[8]=4;wr(p+9,14,4);wr(p+13,12,8);wr(p+21,7,4);p[26]=9;
    b=block_bytes(g,2,1,p,27,27,0,0,0,0,0);
    check("row7_unknown_end",stop_is(view(b),PGWT_CODEC_RECORD,201,b.size,1,8));pgwt_codec_bytes_free(&b);
    /* All framing must pass, including frames AFTER an otherwise readable
     * unknown record. No LIFE from the block may escape the bad second frame. */
    b=copy(g);b.data[273]=99;wr(b.data+302,65535,4);repair(b);
    check("atomic_frames_after_fault",stop_is(view(b),PGWT_CODEC_FRAMING,201,201,0,0));pgwt_codec_bytes_free(&b);
    /* Three outer frames claimed, but only two stored. */
    b=copy(g);wr(b.data+217,3,4);wr(b.data+269,3,4);repair(b);
    check("row4_truncated_frame",stop_is(view(b),PGWT_CODEC_FRAMING,201,201,0,0));pgwt_codec_bytes_free(&b);
    /* LIFE with observed fork after detection time. */
    memset(p,0,sizeof(p));p[0]=1;p[4]=1;p[8]=1;wr(p+9,31,4);wr(p+13,1,8);wr(p+21,7,4);p[25]=1;wr(p+26,2,8);
    wr(p+34,2,8);p[42]=1;p[43]=0;
    b=block_bytes(g,2,1,p,44,44,0,0,0,0,0);
    check("row8_future_fork",stop_is(view(b),PGWT_CODEC_RECORD,201,b.size,1,8));pgwt_codec_bytes_free(&b);
}

static void encoder_validation(pgwt_codec_buffer g)
{
    pgwt_codec_decoded out;if (pgwt_codec_decode(&g,1,&out)) exit(2);
    pgwt_evidence_record r=out.files[0].records[0];pgwt_evidence_file f=out.files[0];f.records=&r;f.record_count=1;
    pgwt_codec_descriptor d=out.info[0].descriptor;pgwt_codec_bytes b={0};
    const struct { const char *name;pgwt_evidence_record r; } bad[]={
        {"writer_unknown_kind",{.kind=PGWT_EVIDENCE_OTHER}},
        {"writer_unsupported",{.kind=PGWT_EVIDENCE_LIFE,.unsupported_shape=true}},
        {"writer_life_pid",{.kind=PGWT_EVIDENCE_LIFE}},
        {"writer_life_start",{.kind=PGWT_EVIDENCE_LIFE,.pid=7,.t=1,.start=2}},
        {"writer_life_fork",{.kind=PGWT_EVIDENCE_LIFE,.pid=7,.t=1,.start_known=true}},
        {"writer_life_exit",{.kind=PGWT_EVIDENCE_LIFE,.pid=7,.t=1,.has_exit=true,.exit=2}},
        {"writer_attach_unknown_counters",{.kind=PGWT_EVIDENCE_ATTACH_OK,.pid=7}},
        {"writer_frontier_unknown_counters",{.kind=PGWT_EVIDENCE_FRONTIER}},
        {"writer_end_reason",{.kind=PGWT_EVIDENCE_END,.pid=7,.end_reason={"future",6}}},
        {"writer_void_reason",{.kind=PGWT_EVIDENCE_VOID}},
        {"writer_trans_duration",{.kind=PGWT_EVIDENCE_TRANS,.t=1,.pid=7,.duration=2,.new_event={"A",1}}},
        {"writer_trans_same",{.kind=PGWT_EVIDENCE_TRANS,.pid=7}},
        {"writer_attach_unmapped",{.kind=PGWT_EVIDENCE_ATTACH_OK,.pid=7,.seed={"missing",7},.counters={.known=true}}}
    };
    for (size_t i=0;i<sizeof(bad)/sizeof(*bad);++i) {
        r=bad[i].r;
        check(bad[i].name,pgwt_codec_encode(&f,&d,NULL,&b,NULL)==-1 && !b.data && !b.size);pgwt_codec_bytes_free(&b);
    }
    r=out.files[0].records[0];
    pgwt_codec_mapping map[2]={{1,1,{"A",1}},{2,1,{"A",1}}};d.mapping=map;d.mapping_count=2;
    check("writer_duplicate_names",pgwt_codec_encode(&f,&d,NULL,&b,NULL)==-1 && !b.data);pgwt_codec_bytes_free(&b);
    map[1].name=(pgwt_core_event){"B",1};map[1].id=1;
    check("writer_mapping_order",pgwt_codec_encode(&f,&d,NULL,&b,NULL)==-1 && !b.data);pgwt_codec_bytes_free(&b);
    d=out.info[0].descriptor;d.block_records=4097;
    check("writer_block_limit",pgwt_codec_encode(&f,&d,NULL,&b,NULL)==-1 && !b.data);
    pgwt_codec_bytes_free(&b);pgwt_codec_decoded_free(&out);
}

static void native_boundaries(void)
{
    static const char opaque[]={'A',0,'B'};
    pgwt_codec_mapping map[]={{1,1,{opaque,sizeof(opaque)}},{UINT32_MAX-1,1,{"Z",1}}};
    pgwt_evidence_record records[]={
        {.kind=PGWT_EVIDENCE_LIFE,.t=1,.pid=7},
        {.kind=PGWT_EVIDENCE_ATTACH_OK,.t=2,.pid=7,.reread=true,.counters={.known=true}},
        {.kind=PGWT_EVIDENCE_TRANS,.t=10,.pid=7,.old_event={opaque,sizeof(opaque)}},
        {.kind=PGWT_EVIDENCE_TRANS,.t=100,.pid=7,.new_event={opaque,sizeof(opaque)}},
        {.kind=PGWT_EVIDENCE_TRANS,.t=20,.pid=7,.old_event={opaque,sizeof(opaque)},.new_event={"$EXIT",5}},
        {.kind=PGWT_EVIDENCE_FRONTIER,.t=101,.counters={.known=true}}
    };
    pgwt_codec_transition metadata[6]={{0},{0},{UINT64_MAX,UINT64_MAX},{123,0},{456,17},{0}};
    pgwt_evidence_file f={.version=5,.boot={opaque,sizeof(opaque)},.capture={"native",6},.present=true,.qualified=true,
        .records=records,.record_count=6,.baseline={.known=true}};
    pgwt_codec_descriptor d={.mapping=map,.mapping_count=2,.transitions=metadata,.footer=true};pgwt_codec_bytes bytes={0};
    if (pgwt_codec_encode(&f,&d,NULL,&bytes,NULL)) exit(2);
    pgwt_codec_buffer b=view(bytes);pgwt_codec_decoded out;int status=pgwt_codec_decode(&b,1,&out);
    bool ok=status==0 && out.files[0].record_count==6 && out.info[0].block_count==3 && out.info[0].descriptor.footer;
    if (ok) {
        const pgwt_evidence_record *r=out.files[0].records;
        ok=r[2].t==10 && r[3].t==100 && r[4].t==20 && r[2].old_event.length==3 &&
            !memcmp(r[2].old_event.data,opaque,3) && r[4].new_event.length==5 && !memcmp(r[4].new_event.data,"$EXIT",5) &&
            out.info[0].descriptor.transitions[2].query_id==UINT64_MAX && out.info[0].descriptor.transitions[2].cpu_ns==UINT64_MAX &&
            out.info[0].descriptor.transitions[3].query_id==123 && out.info[0].descriptor.transitions[4].cpu_ns==17 &&
            out.info[0].positions[2].record_offset==0 && out.info[0].positions[3].record_offset==1 && out.info[0].positions[4].record_offset==2;
    }
    check("late_rows_metadata_opaque",ok);pgwt_codec_decoded_free(&out);pgwt_codec_bytes_free(&bytes);
    /* Minimal descriptor/header and both empty-file forms. */
    f.boot=(pgwt_core_event){"b",1};f.capture=(pgwt_core_event){"c",1};f.baseline.known=false;f.record_count=0;
    d=(pgwt_codec_descriptor){0};
    int rc=pgwt_codec_encode(&f,&d,NULL,&bytes,NULL);
    check("empty_minimum_header",rc==0 && bytes.size==73 && complete_records(view(bytes),0));pgwt_codec_bytes_free(&bytes);
    d.footer=true;rc=pgwt_codec_encode(&f,&d,NULL,&bytes,NULL);
    check("empty_footer",rc==0 && bytes.size==77 && complete_records(view(bytes),0));pgwt_codec_bytes_free(&bytes);
    /* Two equally sized mapping names allow independent wire corruptions. */
    pgwt_codec_mapping two[]={{1,1,{"A",1}},{2,1,{"B",1}}};d=(pgwt_codec_descriptor){.mapping=two,.mapping_count=2};
    if (pgwt_codec_encode(&f,&d,NULL,&bytes,NULL)) exit(2);
    mutate(view(bytes),"row1_duplicate_names",84,'A',1,true,PGWT_CODEC_HEADER,0,0,0,0);
    mutate(view(bytes),"row1_mapping_order",77,1,4,true,PGWT_CODEC_HEADER,0,0,0,0);
    pgwt_codec_bytes_free(&bytes);
    /* Local LIFE accepts observed zero and exit zero. VOID keeps optional
     * zero distinct from absent, preserves opaque diagnostics and all slots. */
    pgwt_evidence_record local[]={
        {.kind=PGWT_EVIDENCE_LIFE,.pid=UINT32_MAX,.has_fork=true,.fork=0,.start_known=true,.has_exit=true,.exit=0},
        {.kind=PGWT_EVIDENCE_VOID,.t=UINT64_MAX,.void_reason={opaque,3},.has_invalid_from=true,.invalid_from=0,.counters={.known=true}}
    };
    for (unsigned i=0;i<15;++i) local[1].counters.value[i]=UINT64_MAX-i;
    f.records=local;f.record_count=2;d=(pgwt_codec_descriptor){0};
    if (pgwt_codec_encode(&f,&d,NULL,&bytes,NULL)) exit(2);
    b=view(bytes);status=pgwt_codec_decode(&b,1,&out);
    ok=status==0 && out.files[0].records[0].has_fork && out.files[0].records[0].fork==0 &&
        out.files[0].records[0].has_exit && out.files[0].records[0].exit==0 && out.files[0].records[1].has_invalid_from &&
        out.files[0].records[1].invalid_from==0 && out.files[0].records[1].void_reason.length==3 &&
        !memcmp(out.files[0].records[1].void_reason.data,opaque,3);
    if (ok) for (unsigned i=0;i<15;++i) ok &= out.files[0].records[1].counters.value[i]==UINT64_MAX-i;
    check("optionals_counter_slots",ok);pgwt_codec_decoded_free(&out);pgwt_codec_bytes_free(&bytes);
    /* Count-bound split: 4097 records must never narrow the count or silently
     * truncate. This is a fixed boundary check, not U7b's every-cut exercise. */
    pgwt_evidence_record *many=calloc(4097,sizeof(*many));if (!many) exit(2);
    for (size_t i=0;i<4097;++i) many[i]=(pgwt_evidence_record){.kind=PGWT_EVIDENCE_FRONTIER,.t=i,.counters={.known=true}};
    f.records=many;f.record_count=4097;
    if (pgwt_codec_encode(&f,&d,NULL,&bytes,NULL)) exit(2);
    b=view(bytes);status=pgwt_codec_decode(&b,1,&out);
    check("count_bound_split",status==0 && out.files[0].record_count==4097 && out.info[0].block_count==2 &&
        out.files[0].records[4096].t==4096);
    pgwt_codec_decoded_free(&out);pgwt_codec_bytes_free(&bytes);
    /* Payload-bound split happens before the count bound for long VOID bodies. */
    char reason[4096];memset(reason,'x',sizeof(reason));
    for (size_t i=0;i<4096;++i) many[i]=(pgwt_evidence_record){.kind=PGWT_EVIDENCE_VOID,.t=i,.void_reason={reason,sizeof(reason)},.counters={.known=true}};
    f.record_count=4096;
    if (pgwt_codec_encode(&f,&d,NULL,&bytes,NULL)) exit(2);
    b=view(bytes);status=pgwt_codec_decode(&b,1,&out);
    check("payload_bound_split",status==0 && out.files[0].record_count==4096 && out.info[0].block_count==2 && out.files[0].records[4095].t==4095);
    pgwt_codec_decoded_free(&out);pgwt_codec_bytes_free(&bytes);free(many);
    pgwt_codec_decoded empty;
    check("api_empty_chain",pgwt_codec_decode(NULL,0,&empty)==0 && !empty.file_count);pgwt_codec_decoded_free(&empty);
    check("api_overflow_count",pgwt_codec_decode(NULL,SIZE_MAX,&empty)==-1 && !empty.file_count);pgwt_codec_decoded_free(&empty);
}

static void null_counter_recovery(void)
{
    pgwt_evidence_record records[]={
        {.kind=PGWT_EVIDENCE_LIFE,.pid=7,.has_fork=true,.start_known=true},
        {.kind=PGWT_EVIDENCE_ATTACH_OK,.pid=7,.has_fork=true,.t=1,.reread=true,.counters={.known=true}},
        {.kind=PGWT_EVIDENCE_TRANS,.pid=7,.t=3,.new_event={"A",1},.duration=2},
        {.kind=PGWT_EVIDENCE_FRONTIER,.t=8,.counters={.known=true}},
        {.kind=PGWT_EVIDENCE_FRONTIER,.t=12,.counters={.known=true}},
        {.kind=PGWT_EVIDENCE_LIFE,.t=15,.pid=7,.has_fork=true,.fork=15,.start=15,.start_known=true},
        {.kind=PGWT_EVIDENCE_ATTACH_OK,.pid=7,.has_fork=true,.fork=15,.t=16,.reread=true,.counters={.known=true}},
        {.kind=PGWT_EVIDENCE_TRANS,.pid=7,.t=18,.new_event={"A",1},.duration=2},
        {.kind=PGWT_EVIDENCE_FRONTIER,.t=24,.counters={.known=true}}
    };
    pgwt_evidence_file f={.version=5,.boot={"b",1},.capture={"c",1},.qualified=true,.present=true,.baseline={.known=true},.records=records,.record_count=9};
    pgwt_codec_mapping mapping={1,1,{"A",1}};pgwt_codec_descriptor d={.mapping=&mapping,.mapping_count=1,.block_records=1};
    pgwt_codec_bytes b={0};if (pgwt_codec_encode(&f,&d,NULL,&b,NULL)) exit(2);
    size_t off=(size_t)rd(b.data+40,4);
    for (size_t i=0;i<3;++i) off+=68+(size_t)rd(b.data+off+20,4);
    /* Replace the valid FRONTIER vector with the single null byte, shrink the
     * bounded body, and relocate/rechain every subsequent physical block. */
    size_t remove=off+64+22;
    memmove(b.data+remove,b.data+remove+120,b.size-remove-120);b.size-=120;
    b.data[off+64+21]=0;wr(b.data+off+64+9,9,4);wr(b.data+off+20,22,4);wr(b.data+off+24,22,4);
    for (size_t at=(size_t)rd(b.data+40,4);at<b.size;at+=68+(size_t)rd(b.data+at+20,4)) wr(b.data+at+48,at,8);
    repair(b);pgwt_codec_buffer v=view(b);pgwt_codec_decoded out;int status=pgwt_codec_decode(&v,1,&out);
    pgwt_evidence_input in={.files=out.files,.file_count=out.file_count,.capture={"c",1},.pid=7,.has_fork=true,.fork=15};
    pgwt_core_query q={.event={"A",1},.from=16,.to=24};pgwt_evidence_output replay={0};
    bool ok=status==0 && out.files[0].record_count==9 && !out.files[0].records[3].counters.known &&
        !out.files[0].records[3].unsupported_shape && pgwt_evidence_replay(&in,&q,&replay,NULL)==PGWT_REPLAY_OK &&
        replay.projection.coverage_count==1 && replay.coverage.start==16 && replay.coverage.end==24 &&
        replay.wait_time.value.present && replay.wait_time.value.value==6 && replay.completed_waits.value.present && replay.completed_waits.value.value==0;
    check("row10_null_counter_recovery",ok);pgwt_evidence_output_free(&replay);pgwt_codec_decoded_free(&out);pgwt_codec_bytes_free(&b);
}

static size_t varint_length(const unsigned char *p)
{ size_t n=1;while (p[n-1]&128) ++n;return n; }
static void map_add(pgwt_codec_mapping *map, size_t *n, pgwt_core_event e)
{
    if (!e.data || !e.length || (e.length==5 && !memcmp(e.data,"$EXIT",5))) return;
    for (size_t i=0;i<*n;++i) if (map[i].name.length==e.length && !memcmp(map[i].name.data,e.data,e.length)) return;
    if (*n==256) exit(2);
    map[*n]=(pgwt_codec_mapping){(uint32_t)*n+1,1,e};++*n;
}
/* Each scenario is built with one record per block to isolate deliberately
 * missing JSON fields. Wire fields are mandatory: scenarios 34/35/54 (missing
 * duration) and 52/53 (missing t) cannot be represented as well-framed records.
 * Remove that required varint column slot from their type-0 block instead.
 * The now undecodable columns reject the segment at the same physical boundary;
 * no other record shares this block, so the retained logical prefix is exact.
 * 36/37 use an unknown framed wire kind. No product encoder emits these faults.
 * Missing file 23 is encoded for digest construction, then withheld entirely.
 * Legacy scenarios 28–30 deliberately use the logical API without v5 encoding.
 */
static void scenario(const char *path, const char *expected, const char *projections)
{
    evidence_case c;char field[512]={0};
    if (evidence_case_load(path,&c,field,sizeof(field))) { fprintf(stderr,"%s\n",field);exit(2); }
    unsigned id=(unsigned)strtoul(c.id,NULL,10);
    pgwt_codec_mapping map[256];size_t maps=0;
    for (size_t i=0;i<c.input.file_count;++i) for (size_t j=0;j<c.files[i].record_count;++j) {
        const pgwt_evidence_record *r=&c.files[i].records[j];
        map_add(map,&maps,r->seed);map_add(map,&maps,r->state);map_add(map,&maps,r->old_event);map_add(map,&maps,r->new_event);
    }
    pgwt_codec_bytes *bytes=calloc(c.input.file_count,sizeof(*bytes));
    pgwt_codec_buffer *buffers=calloc(c.input.file_count,sizeof(*buffers));
    if (!bytes || !buffers) exit(2);
    size_t nb=0;bool ok=true;
    if (id<28 || id>30) for (size_t i=0;i<c.input.file_count && ok;++i) {
        pgwt_evidence_file f=c.files[i];
        pgwt_evidence_record *records=calloc(f.record_count ? f.record_count : 1,sizeof(*records));if (!records) exit(2);
        memcpy(records,f.records,f.record_count*sizeof(*records));f.records=records;f.present=true;
        for (size_t j=0;j<f.record_count;++j) {
            if (records[j].unsupported_shape) {
                records[j].unsupported_shape=false;records[j].t=records[j].t ? records[j].t : 1;
                records[j].pid=records[j].pid ? records[j].pid : 101;records[j].duration=0;
            }
            if (records[j].kind==PGWT_EVIDENCE_OTHER) records[j]=(pgwt_evidence_record){.kind=PGWT_EVIDENCE_FRONTIER,.t=records[j].t,.counters={.known=true}};
            if (records[j].has_fork && records[j].fork>records[j].t) records[j].fork=records[j].t;
        }
        pgwt_codec_descriptor d={.mapping=map,.mapping_count=maps,.block_records=1,.footer=true};
        pgwt_codec_buffer prev={0};
        if (f.file_no) {
            for (size_t k=0;k<i;++k) if (c.files[k].file_no==f.previous &&
                c.files[k].capture.length==f.capture.length && !memcmp(c.files[k].capture.data,f.capture.data,f.capture.length) &&
                c.files[k].boot.length==f.boot.length && !memcmp(c.files[k].boot.data,f.boot.data,f.boot.length)) prev=view(bytes[k]);
            if (!prev.data) exit(2);
        }
        const char *why=NULL;
        if (pgwt_codec_encode(&f,&d,f.file_no ? &prev : NULL,&bytes[i],&why)) {
            fprintf(stderr,"codec scenario %s encode error %s\n",c.id,why);ok=false;
        }
        free(records);
    }
    if (id<28 || id>30) for (size_t i=0;i<c.input.file_count && ok;++i) {
        size_t off=(size_t)rd(bytes[i].data+40,4);
        for (size_t j=0;j<c.files[i].record_count;++j) {
            const pgwt_evidence_record *r=&c.files[i].records[j];unsigned char *p=bytes[i].data+off+64;
            if (r->unsupported_shape) {
                if (r->kind!=PGWT_EVIDENCE_TRANS) exit(2);
                size_t column=(id==52 || id==53) ? 0 : varint_length(p)+12;
                size_t length=varint_length(p+column),at=off+64+column;
                size_t stored=(size_t)rd(bytes[i].data+off+20,4);
                memmove(bytes[i].data+at,bytes[i].data+at+length,bytes[i].size-at-length);
                bytes[i].size-=length;wr(bytes[i].data+off+20,stored-length,4);wr(bytes[i].data+off+24,stored-length,4);
            }
            else if (r->kind==PGWT_EVIDENCE_OTHER) p[8]=99;
            else if (r->has_fork && r->fork>r->t) wr(p+26,r->fork,8);
            off+=68+(size_t)rd(bytes[i].data+off+20,4);
        }
        /* Reconstruct physical offsets/footer after a removed column slot. */
        size_t data_end=off;off=(size_t)rd(bytes[i].data+40,4);
        for (size_t j=0;j<c.files[i].record_count;++j) {
            wr(bytes[i].data+off+48,off,8);wr(bytes[i].data+data_end+16*j+8,off,8);
            off+=68+(size_t)rd(bytes[i].data+off+20,4);
        }
        repair(bytes[i]);
        if (c.files[i].present) buffers[nb++]=view(bytes[i]);
    }
    pgwt_codec_decoded decoded={0};pgwt_evidence_input in=c.input;
    if ((id<28 || id>30) && ok) {
        int status=pgwt_codec_decode(buffers,nb,&decoded);
        ok=status>=0;in.files=decoded.files;in.file_count=decoded.file_count;
    }
    pgwt_evidence_output out={0};const char *reason=NULL;
    if (ok) ok=pgwt_evidence_replay(&in,&c.query,&out,&reason)==PGWT_REPLAY_OK;
    if (ok) ok=evidence_compare(&c,&out,expected,projections,field,sizeof(field))==0;
    char name[256];snprintf(name,sizeof(name),"scenario_%s%s",c.id,id>=28 && id<=30 ? "_legacy_logical" : "_roundtrip");
    if (!ok) fprintf(stderr,"%s %s %s\n",name,field,reason ? reason : "");
    check(name,ok);pgwt_evidence_output_free(&out);pgwt_codec_decoded_free(&decoded);
    for (size_t i=0;i<c.input.file_count;++i) pgwt_codec_bytes_free(&bytes[i]);
    free(bytes);free(buffers);evidence_case_free(&c);
}
static void scenarios(const char *root)
{
    for (unsigned rev=0;rev<2;++rev) {
        char pattern[4096],expected[4096],projections[4096];
        snprintf(pattern,sizeof(pattern),"%s%s/scenarios/*.json",root,rev ? "/r3" : "");
        snprintf(expected,sizeof(expected),"%s%s/expected.json",root,rev ? "/r3" : "");
        snprintf(projections,sizeof(projections),"%s%s/projections.json",root,rev ? "/r3" : "");
        glob_t paths={0};if (glob(pattern,0,NULL,&paths) || paths.gl_pathc!=(rev ? 31 : 33)) exit(2);
        for (size_t i=0;i<paths.gl_pathc;++i) scenario(paths.gl_pathv[i],expected,projections);
        globfree(&paths);
    }
    if (!only) puts("codec scenarios: 64 checked (61 v5 byte paths; 3 legacy logical); 34/35/52/53/54 omit their mandatory varint slot, rejecting that single-record segment at the same boundary");
}

int main(int argc, char **argv)
{
    const char *root="contracts/evidence-v1";
    if (access(root,R_OK)) root="../contracts/evidence-v1";
    const char *write_path=NULL;
    for (int i=1;i<argc;++i) {
        if (!strcmp(argv[i],"--case") && i+1<argc) only=argv[++i];
        else if (!strcmp(argv[i],"--write-golden") && i+1<argc) write_path=argv[++i];
        else root=argv[i];
    }
    pgwt_codec_bytes g=golden(root);known_answers(view(g));
    if (only && (!strcmp(only,"crc_known_answers") || !strcmp(only,"sha256_known_answers"))) {
        pgwt_codec_bytes_free(&g);return failures ? 1 : 0;
    }
    pgwt_codec_bytes encoded=golden_roundtrip(view(g));
    refusal_rows(view(g));predecessor_tests(view(g));compression_samples(view(g));sample_stream(view(g));record_shapes(view(g));encoder_validation(view(g));native_boundaries();null_counter_recovery();scenarios(root);
    if (write_path) {
        FILE *f=fopen(write_path,"wb");if (!f || fwrite(encoded.data,1,encoded.size,f)!=encoded.size || fclose(f)) return 2;
    }
    pgwt_codec_bytes_free(&encoded);pgwt_codec_bytes_free(&g);
    return failures ? 1 : 0;
}
