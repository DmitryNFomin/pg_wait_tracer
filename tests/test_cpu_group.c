#define _GNU_SOURCE
#include "cpu_group.h"
#include <assert.h>
#include <errno.h>
#include <ftw.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static char root[256], proc[300], cg[300];
static unsigned checks;

static void put(const char *relative, const char *text)
{
    char path[1024];
    assert(snprintf(path, sizeof(path), "%s/%s", root, relative) < (int)sizeof(path));
    for (char *p = path + 1; *p; ++p) {
        if (*p == '/') {
            *p = '\0';
            assert(mkdir(path, 0755) == 0 || errno == EEXIST);
            *p = '/';
        }
    }
    FILE *f = fopen(path, "w");
    assert(f);
    assert(fputs(text, f) >= 0);
    assert(fclose(f) == 0);
}

static void expect(const char *name, enum pgwt_cpu_group_relation expected,
                   const char *a_path, const char *b_path, const char *reason)
{
    struct pgwt_cpu_group a, b;
    enum pgwt_cpu_group_relation got = pgwt_cpu_group_compare(proc, cg, 101, 202, &a, &b);
    assert(got == expected);
    if (a_path) assert(!strcmp(a.path, a_path));
    if (b_path) assert(!strcmp(b.path, b_path));
    if (reason) assert(strstr(a.reason, reason) || strstr(b.reason, reason));
    ++checks;
    printf("PASS %s: relation=%d tracer=%s:%s postgres=%s:%s reason=%s%s\n",
           name, got, pgwt_cpu_group_source(&a), a.path,
           pgwt_cpu_group_source(&b), b.path, a.reason, b.reason);
}

static int remove_entry(const char *path, const struct stat *st, int kind, struct FTW *ftw)
{
    (void)st; (void)kind; (void)ftw;
    return remove(path);
}

int main(void)
{
    strcpy(root, "/tmp/pgwt-cpu-group-XXXXXX");
    assert(mkdtemp(root));
    snprintf(proc, sizeof(proc), "%s/proc", root);
    snprintf(cg, sizeof(cg), "%s/cgroup", root);
    put("proc/101/cgroup", "0::/system.slice/tracer.service\n");
    put("proc/202/cgroup", "0::/system.slice/system-postgresql.slice/pg.service\n");
    put("cgroup/cgroup.subtree_control", "cpu memory pids\n");
    put("cgroup/system.slice/cgroup.subtree_control", "memory pids\n");
    put("cgroup/system.slice/system-postgresql.slice/cgroup.subtree_control", "memory\n");
    put("cgroup/system.slice/tracer.service/cgroup.procs", "101\n");
    put("cgroup/system.slice/system-postgresql.slice/pg.service/cgroup.procs", "202\n");
    expect("v2 no cpu below system.slice", PGWT_CPU_GROUP_SAME, "/system.slice", "/system.slice", NULL);
    put("cgroup/system.slice/system-postgresql.slice/cgroup.subtree_control", "cpu memory\n");
    expect("v2 PostgreSQL slice enables cpu", PGWT_CPU_GROUP_DIFFERENT,
           "/system.slice", "/system.slice/system-postgresql.slice/pg.service", NULL);
    put("cgroup/system.slice/system-postgresql.slice/cgroup.subtree_control", "memory\n");
    put("cgroup/system.slice/cgroup.subtree_control", "cpu memory pids\n");
    expect("v2 tracer CPU property enables siblings", PGWT_CPU_GROUP_DIFFERENT,
           "/system.slice/tracer.service", "/system.slice/system-postgresql.slice", NULL);
    put("cgroup/system.slice/cgroup.subtree_control", "memory\n");
    put("cgroup/cgroup.subtree_control", "memory pids\n");
    expect("v2 no cpu anywhere", PGWT_CPU_GROUP_SAME, "/", "/", NULL);
    put("cgroup/system.slice/cgroup.subtree_control", "cpuacct cpuset\n");
    expect("cpuacct/cpuset are not cpu", PGWT_CPU_GROUP_SAME, "/", "/", NULL);
    put("proc/101/cgroup", "4:cpuacct,cpu:/system.slice\n2:memory:/tracer\n");
    put("proc/202/cgroup", "4:cpuacct,cpu:/system.slice\n2:memory:/postgres\n");
    expect("v1 same path", PGWT_CPU_GROUP_SAME, "/system.slice", "/system.slice", NULL);
    put("proc/101/cgroup", "4:cpu,cpuacct:/\n");
    expect("v1 root vs system.slice", PGWT_CPU_GROUP_DIFFERENT, "/", "/system.slice", NULL);
    put("proc/101/cgroup", "4:cpu,cpuacct:/system.slice\n0::/other/tracer\n");
    put("proc/202/cgroup", "0::/other/postgres\n4:cpu,cpuacct:/system.slice\n");
    expect("hybrid v1 owns CPU", PGWT_CPU_GROUP_SAME, "/system.slice", "/system.slice", NULL);
    struct pgwt_cpu_group hybrid = pgwt_cpu_group_read(proc, cg, 101);
    assert(hybrid.hybrid && hybrid.version == 1 && strstr(pgwt_cpu_group_source(&hybrid), "owns"));
    /* An existing input that cannot be read as text, even as root; distinct
     * from the vanished process's ENOENT. */
    char file[400];
    snprintf(file, sizeof(file), "%s/101/cgroup", proc);
    assert(unlink(file) == 0 && mkdir(file, 0755) == 0);
    expect("unreadable file", PGWT_CPU_GROUP_UNKNOWN, NULL, NULL, "cannot read");
    assert(rmdir(file) == 0);
    expect("vanished process", PGWT_CPU_GROUP_UNKNOWN, NULL, NULL, "No such file");
    put("proc/101/cgroup", "4:cpu:/system.slice\n");
    assert(chmod(root, 0755) == 0 && chmod(file, 0000) == 0);
    fflush(stdout);
    pid_t child = fork();
    assert(child >= 0);
    if (child == 0) {
        if (geteuid() == 0) assert(setuid(65534) == 0);
        expect("permission-denied file", PGWT_CPU_GROUP_UNKNOWN, NULL, NULL, "Permission denied");
        fflush(stdout);
        _exit(0);
    }
    int status;
    assert(waitpid(child, &status, 0) == child && WIFEXITED(status) && !WEXITSTATUS(status));
    ++checks;
    assert(chmod(file, 0644) == 0);
    put("proc/101/cgroup", "not a cgroup line\n");
    expect("malformed line", PGWT_CPU_GROUP_UNKNOWN, NULL, NULL, "malformed line 1");
    put("proc/101/cgroup", "0::/system.slice/tracer.service\nbroken\n");
    expect("unrelated malformed line", PGWT_CPU_GROUP_UNKNOWN, NULL, NULL, "malformed line 2");
    put("proc/101/cgroup", "0::/system.slice/../tracer\n");
    expect("path traversal", PGWT_CPU_GROUP_UNKNOWN, NULL, NULL, "malformed line");
    put("proc/101/cgroup", "0::/one\n0::/two\n");
    expect("duplicate v2 hierarchy", PGWT_CPU_GROUP_UNKNOWN, NULL, NULL, "malformed line 2");
    put("proc/101/cgroup", "3:cpuacct:/\n");
    expect("no CPU hierarchy", PGWT_CPU_GROUP_UNKNOWN, NULL, NULL, "no CPU hierarchy");
    put("proc/101/cgroup", "0::/missing/leaf\n");
    expect("vanished cgroup directory", PGWT_CPU_GROUP_UNKNOWN, NULL, NULL, "cannot read cgroup directory");
    put("cgroup/missing/leaf/cgroup.procs", "101\n");
    expect("unreadable ancestor", PGWT_CPU_GROUP_UNKNOWN, NULL, NULL, "/missing/cgroup.subtree_control");
    put("proc/101/cgroup", "4:cpu:/system.slice (deleted)\n");
    expect("deleted cgroup", PGWT_CPU_GROUP_UNKNOWN, NULL, NULL, "malformed line");
    put("proc/101/cgroup", "0::/system.slice/tracer.service\n");
    put("cgroup/system.slice/cgroup.subtree_control", "cpu:broken\n");
    expect("malformed controller list", PGWT_CPU_GROUP_UNKNOWN, NULL, NULL, "malformed controller list");
    assert(nftw(root, remove_entry, 32, FTW_DEPTH | FTW_PHYS) == 0);
    printf("%u CPU-group fixture cases passed\n", checks);
    return 0;
}
