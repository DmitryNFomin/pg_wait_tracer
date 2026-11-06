#include "../demos/replay-compat.h"
/* The pacing unit exercises only sampler's pure build/substitution helpers.
 * macOS has no process_vm_readv; read helpers must fail, never fabricate data. */
#ifdef __APPLE__
#include <errno.h>
#include <sys/types.h>
#include <sys/uio.h>
static inline ssize_t process_vm_readv(pid_t pid, const struct iovec *local,
    unsigned long nlocal, const struct iovec *remote, unsigned long nremote,
    unsigned long flags)
{
    (void)pid; (void)local; (void)nlocal; (void)remote; (void)nremote; (void)flags;
    errno = ENOSYS;
    return -1;
}
#endif
