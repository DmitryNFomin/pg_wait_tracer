/* Registered portable tests; Linux also builds the independent --probe loader.
 * make check defines PGWT_TRANSPORT_HOST_TEST even on Linux: never load BPF
 * from a host check. Live entry is test_transport_producers_live.py. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "../src/bpf/transport_producer.h"
#include "transport_probe_decode.h"
#define REQUIRE(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); return 1; } } while (0)
#if defined(__linux__) && !defined(PGWT_TRANSPORT_HOST_TEST)
#include "transport_probe.h"
#endif
static int portable(void)
{
    struct pgwt_wire_hdr hdr;
    memset(&hdr, 0xff, sizeof(hdr));
    pgwt_producer_header(&hdr, PGWT_WIRE_TRACE, 48, 17, 2, 99);
    static const unsigned char golden[32] = {1,0,1,0,48,0,17,0,2,0,0,0,0,0,0,0,
        99,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0};
    REQUIRE(memcmp(&hdr, golden, sizeof(golden)) == 0);
    unsigned char perf_raw[84] = {0};
    memcpy(perf_raw, &hdr, sizeof(hdr));
    REQUIRE(probe_perf_wire_size(perf_raw, 84) == 80);
    REQUIRE(probe_perf_wire_size(perf_raw, 80) == 0);
    REQUIRE(probe_perf_wire_size(perf_raw, 31) == 0);
    hdr.kind=PGWT_WIRE_BOUNDARY; hdr.payload_len=16;
    memcpy(perf_raw, &hdr, sizeof(hdr));
    REQUIRE(probe_perf_wire_size(perf_raw, 52) == 48);
    REQUIRE(probe_perf_wire_size(perf_raw, 48) == 0);
    u64 attempts[3] = {0};
    REQUIRE(pgwt_producer_attempt(&attempts[0]) == 1);
    /* Failed publication consumes the first attempt, another dest is separate. */
    REQUIRE(pgwt_producer_attempt(&attempts[1]) == 1);
    REQUIRE(pgwt_producer_attempt(&attempts[0]) == 2 && attempts[2] == 0);
    u64 authority = 0;
    REQUIRE(pgwt_producer_allocate(&authority) == 1);
    REQUIRE(pgwt_producer_allocate(&authority) == 2);
    authority = ~(u64)0;
    REQUIRE(pgwt_producer_allocate(&authority) == 0 && authority == ~(u64)0);
    struct pgwt_pid_state state = {.cookie=9};
    REQUIRE(pgwt_producer_cookie(&state) == 9 && pgwt_producer_cookie(NULL) == 0);
    state.cookie = 0;
    REQUIRE(pgwt_producer_cookie(&state) == 0);
    u32 cpu = 3;
    REQUIRE(pgwt_producer_helper((u64)7<<32|8, 7, &cpu, 3));
    REQUIRE(!pgwt_producer_helper((u64)7<<32|8, 7, &cpu, 4));
    REQUIRE(!pgwt_producer_helper((u64)7<<32|8, 9, &cpu, 3));
    REQUIRE(!pgwt_producer_helper((u64)7<<32|8, 7, NULL, 3));
    puts("transport pure pieces: PASS (golden envelope, attempts, allocation, cookie, CPU binding)");
    fflush(stdout);
    /* Both registered invocations are supported: check.sh runs from root,
     * tests/Makefile and run_all run from tests/. No user-derived shell text. */
    const char *command = access("tests/transport_producers_host.py", R_OK) == 0
        ? "python3 -B tests/transport_producers_host.py"
        : "python3 -B transport_producers_host.py";
    REQUIRE(system(command) == 0);
    return 0;
}
int main(int argc, char **argv)
{
    (void)argv;
    if (argc > 1) {
#if defined(__linux__) && !defined(PGWT_TRANSPORT_HOST_TEST)
        if (!strcmp(argv[1], "--probe")) return transport_probe(argc-1, argv+1);
#endif
        fprintf(stderr, "unsupported arguments (BPF probe requires Linux live build)\n");
        return 2;
    }
    return portable();
}
