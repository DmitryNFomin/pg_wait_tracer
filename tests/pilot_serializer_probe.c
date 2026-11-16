/* Exercise the real C serializer, including every optional and detail field. */
#define main pilot_original_server_main
#include "../src/server.c"
#undef main
#include <assert.h>

static void identity(cJSON *obj, const char *key)
{
    cJSON *value=cJSON_GetObjectItemCaseSensitive(obj,key);
    assert(cJSON_IsString(value));
    assert(!strcmp(value->valuestring,"18446744073709551615"));
}
int main(void)
{
    pgwt_core_optional_u64 max={true,UINT64_MAX};
    pgwt_core_result result={.metric="wait_time",.version=1,.value=max,.unit="ns",
        .basis="observed",.completeness="complete",.detail_present=true,
        .detail={UINT64_MAX,UINT64_MAX},.diagnostic={max,max,max}};
    cJSON *obj=pilot_metric(&result);
    identity(obj,"value");
    cJSON *detail=cJSON_GetObjectItemCaseSensitive(obj,"detail");
    identity(detail,"closed_ns");identity(detail,"open_ns");
    cJSON *diagnostic=cJSON_GetObjectItemCaseSensitive(obj,"diagnostic");
    identity(diagnostic,"recorded_ns");identity(diagnostic,"covered_ns");identity(diagnostic,"required_ns");
    cJSON_Delete(obj);
    result.metric="completed_waits";result.unit="count";result.detail_present=false;
    obj=pilot_metric(&result);
    identity(obj,"value");identity(cJSON_GetObjectItemCaseSensitive(obj,"diagnostic"),"recorded_count");
    assert(!cJSON_GetObjectItemCaseSensitive(obj,"detail"));cJSON_Delete(obj);
    result.value.present=false;result.diagnostic.recorded.present=false;
    obj=pilot_metric(&result);
    assert(cJSON_IsNull(cJSON_GetObjectItemCaseSensitive(obj,"value")));
    assert(cJSON_IsNull(cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(obj,"diagnostic"),"recorded_count")));
    cJSON_Delete(obj);
    puts("C serializer UINT64_MAX identity: PASS");
    return 0;
}
