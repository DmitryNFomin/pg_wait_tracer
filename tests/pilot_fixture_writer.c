#define _POSIX_C_SOURCE 200809L
#include "core/evidence_codec.h"
#include "evidence_json.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Test driver: encode the logical input UNMODIFIED, never expected answers.
 * All bytes must encode before any file is published. Refusal is exit 3. */
static bool same(pgwt_core_event a, pgwt_core_event b)
{ return a.length==b.length && (!a.length || !memcmp(a.data,b.data,a.length)); }
static void mapping(pgwt_codec_mapping *map, size_t *count, pgwt_core_event name)
{
    if (!name.data || !name.length || (name.length==5 && !memcmp(name.data,"$EXIT",5))) return;
    for (size_t i=0;i<*count;++i) if (same(map[i].name,name)) return;
    if (*count==256) exit(2);
    map[*count]=(pgwt_codec_mapping){.id=(uint32_t)*count+1,.role=1,.name=name};
    ++*count;
}
int main(int argc, char **argv)
{
    if (argc!=4) { fprintf(stderr,"usage: pilot_fixture_writer scenario directory block_records\n");return 2; }
    size_t block_records=(size_t)strtoul(argv[3],NULL,10);
    if (block_records!=1 && block_records!=4096) return 2;
    evidence_case c;char error[512];
    if (evidence_case_load(argv[1],&c,error,sizeof(error))) { fprintf(stderr,"%s\n",error);return 2; }
    pgwt_codec_mapping map[256];size_t maps=0;
    for (size_t i=0;i<c.input.file_count;++i) for (size_t j=0;j<c.files[i].record_count;++j) {
        const pgwt_evidence_record *r=&c.files[i].records[j];
        mapping(map,&maps,r->seed);mapping(map,&maps,r->state);
        mapping(map,&maps,r->old_event);mapping(map,&maps,r->new_event);
    }
    pgwt_codec_bytes *bytes=calloc(c.input.file_count+1,sizeof(*bytes));
    if (!bytes) return 2;
    int status=0;
    for (size_t i=0;i<c.input.file_count;++i) {
        const pgwt_evidence_file *f=&c.files[i];
        pgwt_codec_descriptor d={.mapping=map,.mapping_count=maps,.block_records=block_records,.footer=true};
        pgwt_codec_buffer prev={0};
        if (f->file_no) for (size_t k=0;k<i;++k) if (c.files[k].file_no==f->previous &&
            same(c.files[k].capture,f->capture) && same(c.files[k].boot,f->boot))
                prev=(pgwt_codec_buffer){bytes[k].data,bytes[k].size};
        const char *why="predecessor missing";
        if ((f->file_no && !prev.data) || pgwt_codec_encode(f,&d,f->file_no ? &prev : NULL,&bytes[i],&why)) {
            fprintf(stderr,"REFUSED %s: %s\n",c.id,why ? why : "encoder refusal");status=3;break;
        }
    }
    if (!status) for (size_t i=0;i<c.input.file_count;++i) {
        char path[4096];
        snprintf(path,sizeof(path),"%s/pilot-%03zu.trace",argv[2],i);
        FILE *fp=fopen(path,"wb");
        if (!fp) { status=2;break; }
        bool ok=fwrite(bytes[i].data,1,bytes[i].size,fp)==bytes[i].size;
        if (fclose(fp)) ok=false;
        if (!ok) { status=2;break; }
    }
    for (size_t i=0;i<c.input.file_count;++i) pgwt_codec_bytes_free(&bytes[i]);
    free(bytes);evidence_case_free(&c);return status;
}
