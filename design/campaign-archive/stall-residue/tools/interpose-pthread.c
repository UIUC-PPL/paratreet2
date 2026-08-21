/* relay41: name the library that creates the core-stealing thread.
 *
 * relay40 established that in every process one extra thread is PINNED to the
 * same single core as a Charm PE, because pthread_create inherits the
 * creator's affinity mask and the thread is created lazily from inside an
 * already-pinned worker.  The thread's comm is just the process name, which
 * identifies nobody -- comm is INHERITED from the creating thread, and the
 * process's initial comm is the executable basename set at exec().  So a
 * library that never calls pthread_setname_np leaves its threads wearing the
 * application's name.
 *
 * This LD_PRELOAD shim interposes pthread_create and, for every call, records:
 *   - which .so the START ROUTINE lives in            <-- this names the owner
 *   - the caller's backtrace, resolved per frame to (library, symbol)
 *   - the CREATING thread's affinity mask, so we can see it inherit a
 *     single-CPU mask from a pinned PE
 *   - the creating thread's current CPU and tid
 *
 * Deliberately avoids backtrace_symbols(), which allocates; frames are
 * resolved one at a time with dladdr into a stack buffer.
 */
#define _GNU_SOURCE
#include <pthread.h>
#include <dlfcn.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <execinfo.h>
#include <sys/syscall.h>

static int (*real_create)(pthread_t *, const pthread_attr_t *,
                          void *(*)(void *), void *) = NULL;
static FILE *logf = NULL;
static pthread_mutex_t lk = PTHREAD_MUTEX_INITIALIZER;
static int seq = 0;

static const char *base(const char *p)
{
    if (!p) return "?";
    const char *s = strrchr(p, '/');
    return s ? s + 1 : p;
}

static void open_log(void)
{
    if (logf) return;
    const char *dir = getenv("PTHREAD_TRACE_DIR");
    if (!dir) dir = "/tmp";
    char path[512];
    snprintf(path, sizeof path, "%s/pcreate.%d.txt", dir, (int)getpid());
    logf = fopen(path, "a");
    if (logf) {
        char exe[256] = {0};
        ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
        if (n < 0) exe[0] = 0;
        fprintf(logf, "### pid %d exe %s\n", (int)getpid(), exe);
        fflush(logf);
    }
}

/* Render this thread's affinity as either "cpu N" (pinned) or a count. */
static void aff(char *out, size_t n)
{
    cpu_set_t set;
    CPU_ZERO(&set);
    if (sched_getaffinity(0, sizeof set, &set) != 0) {
        snprintf(out, n, "affinity=?");
        return;
    }
    int cnt = CPU_COUNT(&set), only = -1;
    if (cnt == 1)
        for (int i = 0; i < CPU_SETSIZE; i++)
            if (CPU_ISSET(i, &set)) { only = i; break; }
    if (only >= 0) snprintf(out, n, "PINNED to cpu %d", only);
    else           snprintf(out, n, "%d cpus allowed", cnt);
}

int pthread_create(pthread_t *th, const pthread_attr_t *attr,
                   void *(*start)(void *), void *arg)
{
    if (!real_create)
        real_create = (int (*)(pthread_t *, const pthread_attr_t *,
                               void *(*)(void *), void *))
                      dlsym(RTLD_NEXT, "pthread_create");

    pthread_mutex_lock(&lk);
    open_log();
    if (logf) {
        int id = ++seq;
        char a[64];
        aff(a, sizeof a);
        Dl_info di;
        const char *owner = "?", *sym = "?";
        if (dladdr((void *)start, &di)) {
            owner = base(di.dli_fname);
            if (di.dli_sname) sym = di.dli_sname;
        }
        fprintf(logf,
                "\n[%d] pthread_create  START_ROUTINE in %s  (%s)\n"
                "     creator tid %ld  running on cpu %d  %s\n",
                id, owner, sym, (long)syscall(SYS_gettid), sched_getcpu(), a);
        void *bt[24];
        int nf = backtrace(bt, 24);
        for (int i = 1; i < nf; i++) {        /* frame 0 is this shim */
            Dl_info f;
            if (dladdr(bt[i], &f))
                fprintf(logf, "       #%-2d %-24s %s\n", i - 1,
                        base(f.dli_fname), f.dli_sname ? f.dli_sname : "-");
            else
                fprintf(logf, "       #%-2d %p\n", i - 1, bt[i]);
        }
        fflush(logf);
    }
    pthread_mutex_unlock(&lk);

    return real_create(th, attr, start, arg);
}
