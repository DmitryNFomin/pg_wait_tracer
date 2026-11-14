/* A1 owns this registered test slot. No producer exists in U1b-0.
 * The Linux recipe already supplies skeleton/libbpf for a same-file
 * --probe loader; default invocation and Mac compilation stay portable.
 * scripts/check.sh defines PGWT_TRANSPORT_HOST_TEST, including on Linux;
 * keep the probe loader out of that host-only compilation. */
#include "../src/pgwt_transport_wire.h"
#include <stdio.h>

int main(void)
{
    puts("transport producers: placeholder PASS (A1 pending; no qualification)");
    return 0;
}
