/* B owns this registered test slot. Only the precursor refusal is tested. */
#include "../src/percpu_transport.h"
#include "../src/merge_capture.h"
#include <errno.h>
#include <stdio.h>

int main(void)
{
    if (pgwt_percpu_transport_prepare(NULL) != -ENOSYS)
        return 1;
    puts("per-CPU transport: stub refusal PASS (B pending; no qualification)");
    return 0;
}
