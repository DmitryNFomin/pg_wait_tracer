/* Portable tests link the real timing, lifecycle and accumulation code.
 * They never call the Linux capture reader; fail loudly if that changes. */
#ifdef __APPLE__
#include <stdlib.h>
#include <sys/types.h>
#include <sys/uio.h>
static inline ssize_t process_vm_readv(pid_t pid, const struct iovec *local,
    unsigned long nl, const struct iovec *remote, unsigned long nr,
    unsigned long flags)
{
    (void)pid; (void)local; (void)nl; (void)remote; (void)nr; (void)flags;
    abort();
}
#endif
