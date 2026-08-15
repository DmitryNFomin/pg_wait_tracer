/* backend_meta.c — Write backend metadata to backends.jsonl */
#include "backend_meta.h"
#include "cJSON.h"
#include <string.h>
#include <stdio.h>

int pgwt_bm_init(struct pgwt_backend_meta_writer *bm, const char *trace_dir)
{
    memset(bm, 0, sizeof(*bm));
    snprintf(bm->trace_dir, sizeof(bm->trace_dir), "%s", trace_dir);

    char path[512];
    snprintf(path, sizeof(path), "%s/backends.jsonl", trace_dir);
    bm->fp = fopen(path, "ae");
    if (!bm->fp) {
        perror("fopen backends.jsonl");
        return -1;
    }
    return 0;
}

void pgwt_bm_write_context(struct pgwt_backend_meta_writer *bm,
                           pid_t pid, const struct pgwt_metadata *meta,
                           uint32_t databaseid, uint32_t userid)
{
    if (!bm->fp) return;

    const char *type = pgwt_backend_type_name(meta->backend_type);

    cJSON *obj = cJSON_CreateObject();
    cJSON_AddNumberToObject(obj, "pid", pid);
    cJSON_AddStringToObject(obj, "type", type);
    if (meta->usename[0])
        cJSON_AddStringToObject(obj, "user", meta->usename);
    if (meta->datname[0])
        cJSON_AddStringToObject(obj, "db", meta->datname);
    if (meta->client_addr[0])
        cJSON_AddStringToObject(obj, "addr", meta->client_addr);
    if (meta->leader_pid > 0)
        cJSON_AddNumberToObject(obj, "leader_pid", meta->leader_pid);
    if (databaseid > 0)
        cJSON_AddNumberToObject(obj, "dbid", databaseid);
    if (userid > 0)
        cJSON_AddNumberToObject(obj, "userid", userid);

    char *str = cJSON_PrintUnformatted(obj);
    if (str) {
        fprintf(bm->fp, "%s\n", str);
        cJSON_free(str);
    }
    cJSON_Delete(obj);
    fflush(bm->fp);
}

void pgwt_bm_write(struct pgwt_backend_meta_writer *bm,
                   pid_t pid, const struct pgwt_metadata *meta)
{
    pgwt_bm_write_context(bm, pid, meta, 0, 0);
}

void pgwt_bm_observe_context(struct pgwt_backend_meta_writer *bm,
                             struct pgwt_backend *be,
                             uint32_t databaseid, uint32_t userid)
{
    if (!be || !databaseid || !userid)
        return;
    be->databaseid = databaseid;
    be->userid = userid;
    if (bm && be->meta_parsed)
        pgwt_bm_write_context(bm, be->pid, &be->meta, databaseid, userid);
}

void pgwt_bm_close(struct pgwt_backend_meta_writer *bm)
{
    if (bm->fp) {
        fclose(bm->fp);
        bm->fp = NULL;
    }
}
