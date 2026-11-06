/* Deterministic live positive controls for Refs #804. This is a test-only
 * dynamic background worker, compiled against the target server's headers.
 * CPU mode has wei==0. Lock mode requests a real advisory lock held by the
 * harness: PostgreSQL itself instruments that outstanding resource request.
 * Neither path calls a select/pselect6 100ms timer. */
#include "postgres.h"
#include "fmgr.h"
#include "access/xact.h"
#include "executor/spi.h"
#include "libpq/pqsignal.h"
#include "miscadmin.h"
#include "postmaster/bgworker.h"
#include "tcop/tcopprot.h"
#include "storage/ipc.h"
#include "utils/builtins.h"
#include "utils/snapmgr.h"
#include <time.h>

PG_MODULE_MAGIC;
PG_FUNCTION_INFO_V1(pgwt804_launch);
PGDLLEXPORT void pgwt804_main(Datum arg);

Datum pgwt804_launch(PG_FUNCTION_ARGS)
{
    BackgroundWorker worker = {0};
    BackgroundWorkerHandle *handle;
    pid_t pid;
    char *library = text_to_cstring(PG_GETARG_TEXT_PP(0));
    if (strlen(library) >= sizeof(worker.bgw_library_name))
        elog(ERROR, "test library path too long");
    worker.bgw_flags = BGWORKER_SHMEM_ACCESS | BGWORKER_BACKEND_DATABASE_CONNECTION;
    worker.bgw_start_time = BgWorkerStart_RecoveryFinished;
    worker.bgw_restart_time = BGW_NEVER_RESTART;
    snprintf(worker.bgw_library_name, sizeof(worker.bgw_library_name), "%s", library);
    snprintf(worker.bgw_function_name, sizeof(worker.bgw_function_name), "pgwt804_main");
    snprintf(worker.bgw_name, sizeof(worker.bgw_name), "pgwt804 control");
    snprintf(worker.bgw_type, sizeof(worker.bgw_type), "pgwt804 control");
    worker.bgw_main_arg = Int32GetDatum(PG_GETARG_INT32(1));
    worker.bgw_notify_pid = MyProcPid;
    if (!RegisterDynamicBackgroundWorker(&worker, &handle) ||
        WaitForBackgroundWorkerStartup(handle, &pid) != BGWH_STARTED)
        elog(ERROR, "could not start pgwt804 control worker");
    PG_RETURN_INT32(pid);
}

void pgwt804_main(Datum arg)
{
    pqsignal(SIGTERM, die);
    BackgroundWorkerUnblockSignals();
    BackgroundWorkerInitializeConnection("postgres", NULL, 0);
    if (DatumGetInt32(arg) == 3) {
        /* Syscall-identical to the checkpointer's pacing sleep (pg_usleep
         * of 100000 us is select/pselect6 with nfds 0 and a 100 ms
         * timeout), issued by a NON-checkpointer. An over-wide probe filter
         * would misclassify exactly this as CheckpointWriteDelay. */
        struct timespec start, now;
        clock_gettime(CLOCK_MONOTONIC, &start);
        do {
            pg_usleep(100000L);
            CHECK_FOR_INTERRUPTS();
            clock_gettime(CLOCK_MONOTONIC, &now);
        } while (now.tv_sec - start.tv_sec < 30);
    } else if (DatumGetInt32(arg) == 1) {
        struct timespec start, now;
        volatile uint64 x = 1;
        clock_gettime(CLOCK_MONOTONIC, &start);
        do {
            for (unsigned i = 0; i < 100000; i++) x = x * 6364136223846793005ULL + 1;
            CHECK_FOR_INTERRUPTS();
            clock_gettime(CLOCK_MONOTONIC, &now);
        } while (now.tv_sec - start.tv_sec < 30);
    } else {
        StartTransactionCommand();
        SPI_connect();
        PushActiveSnapshot(GetTransactionSnapshot());
        SPI_execute("SELECT pg_advisory_lock(804804)", false, 0);
        PopActiveSnapshot();
        SPI_finish();
        CommitTransactionCommand();
    }
    proc_exit(0);
}
