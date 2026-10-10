/* Developer-only CPU sampler for torchlightd, loaded with LD_PRELOAD on
 * machines where perf is locked (kernel.perf_event_paranoid above 2).
 *
 * Every thread gets a CLOCK_THREAD_CPUTIME_ID timer that delivers SIGPROF
 * after each SAMPLER_PERIOD_NS of that thread's own CPU time; the handler
 * copies the interrupted frame-pointer chain into a preallocated buffer. Build
 * the daemon with -fno-omit-frame-pointer -mno-omit-leaf-frame-pointer.
 * Threads are named by their start routine (pthread_create is wrapped).
 *
 * Commands arrive one per line on the FIFO named by SAMPLER_CTL:
 *   dump PATH      write threads, executable mappings and samples, then reset
 *   reset          discard samples taken so far
 *   trim PATH      malloc_trim(0), writing "rss_before_kib rss_after_kib"
 *   mallinfo PATH  malloc_info XML (one <heap> per arena)
 * tests/bench/profiler/analyze.py symbolizes dumps. Never linked into a shipped
 * binary: process-wide state is inherent to a preloaded signal handler. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE /* REG_RIP, pthread_getattr_np, RTLD_NEXT */
#endif
#include <dlfcn.h>
#include <malloc.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>
#define SAMPLER_DEPTH 40
/* About 50 MB of samples: 20 minutes of one busy thread at 500 Hz. */
#define SAMPLER_CAPACITY 600000
/* 2 ms of thread CPU time per sample, i.e. 500 Hz while a thread is busy. */
#define SAMPLER_PERIOD_NS 2000000
#define SAMPLER_MAX_THREADS 256
/* Lets in-flight handlers finish before a dump reads the buffer. */
#define SAMPLER_QUIESCE_US 20000
#define SAMPLER_LINE_BYTES 1024
#define SAMPLER_FRAME_ALIGNMENT 8
#ifndef sigev_notify_thread_id
#define sigev_notify_thread_id _sigev_un._tid
#endif
/* One stack sample; the dump writes these records verbatim (little endian). */
struct sample {
    uint32_t tid, depth;
    uint64_t frames[SAMPLER_DEPTH];
};
struct thread_name {
    uint32_t tid;
    uint64_t start;
};
struct trampoline {
    void *(*start)(void *);
    void *argument;
};
typedef int (*create_function)(pthread_t *, const pthread_attr_t *, void *(*)(void *), void *);
static struct sample *samples;
static atomic_ulong sample_count;
static atomic_int recording = 1;
static struct thread_name threads[SAMPLER_MAX_THREADS];
static atomic_int thread_count;
static create_function real_create;
/* Initial-exec TLS is async-signal-safe to read from the handler. */
static __thread __attribute__((tls_model("initial-exec"))) uintptr_t stack_low, stack_high;
static __thread __attribute__((tls_model("initial-exec"))) uint32_t thread_id;
/* Follow saved frame pointers within this thread's stack, starting at the
 * interrupted instruction. Stops at the first frame that leaves the stack. */
static uint32_t walk_frames(const ucontext_t *context, uint64_t *frames) {
#if defined(__x86_64__)
    uint32_t depth = 0;
    frames[depth++] = (uint64_t)context->uc_mcontext.gregs[REG_RIP];
    uintptr_t frame = (uintptr_t)context->uc_mcontext.gregs[REG_RBP];
    while (depth < SAMPLER_DEPTH && frame >= stack_low &&
           frame + 2 * sizeof(uintptr_t) <= stack_high && frame % SAMPLER_FRAME_ALIGNMENT == 0) {
        const uintptr_t *saved = (const uintptr_t *)frame;
        if (saved[1] == 0)
            break;
        frames[depth++] = saved[1];
        if (saved[0] <= frame)
            break;
        frame = saved[0];
    }
    return depth;
#else
    (void)context;
    (void)frames;
    return 0;
#endif
}
static void on_sample(int signal_number, siginfo_t *info, void *context) {
    (void)signal_number;
    (void)info;
    if (!atomic_load(&recording) || samples == NULL)
        return;
    unsigned long index = atomic_fetch_add(&sample_count, 1);
    if (index >= SAMPLER_CAPACITY)
        return;
    struct sample *sample = &samples[index];
    sample->tid = thread_id;
    sample->depth = walk_frames(context, sample->frames);
}
/* Record this thread's stack bounds and name, then arm its CPU-time timer. */
static void arm_thread(uint64_t start) {
    thread_id = (uint32_t)syscall(SYS_gettid);
    pthread_attr_t attributes;
    if (pthread_getattr_np(pthread_self(), &attributes) == 0) {
        void *address = NULL;
        size_t size = 0;
        if (pthread_attr_getstack(&attributes, &address, &size) == 0) {
            stack_low = (uintptr_t)address;
            stack_high = (uintptr_t)address + size;
        }
        pthread_attr_destroy(&attributes);
    }
    int slot = atomic_fetch_add(&thread_count, 1);
    if (slot < SAMPLER_MAX_THREADS)
        threads[slot] = (struct thread_name){thread_id, start};
    struct sigevent event = {.sigev_notify = SIGEV_THREAD_ID, .sigev_signo = SIGPROF};
    event.sigev_notify_thread_id = (int)thread_id;
    timer_t timer;
    if (timer_create(CLOCK_THREAD_CPUTIME_ID, &event, &timer) != 0)
        return;
    struct itimerspec period = {{0, SAMPLER_PERIOD_NS}, {0, SAMPLER_PERIOD_NS}};
    if (timer_settime(timer, 0, &period, NULL) != 0)
        timer_delete(timer);
}
static void *run_thread(void *context) {
    struct trampoline trampoline = *(struct trampoline *)context;
    free(context);
    arm_thread((uint64_t)(uintptr_t)trampoline.start);
    return trampoline.start(trampoline.argument);
}
static create_function original_create(void) {
    if (real_create == NULL) {
        void *symbol = dlsym(RTLD_NEXT, "pthread_create");
        _Static_assert(sizeof(real_create) == sizeof(symbol), "ELF function pointer size");
        memcpy(&real_create, &symbol, sizeof(symbol));
    }
    return real_create;
}
/* Interposed so every thread the daemon starts is armed and named. */
int pthread_create(pthread_t *thread, const pthread_attr_t *attributes, void *(*start)(void *),
                   void *argument) {
    create_function create = original_create();
    struct trampoline *trampoline = malloc(sizeof(*trampoline));
    if (trampoline == NULL)
        return create(thread, attributes, start, argument);
    *trampoline = (struct trampoline){start, argument};
    int code = create(thread, attributes, run_thread, trampoline);
    if (code != 0)
        free(trampoline);
    return code;
}
static long rss_kib(void) {
    FILE *status = fopen("/proc/self/status", "r");
    if (status == NULL)
        return -1;
    char line[SAMPLER_LINE_BYTES];
    long value = -1;
    while (fgets(line, sizeof(line), status) != NULL)
        if (strncmp(line, "VmRSS:", strlen("VmRSS:")) == 0)
            value = strtol(line + strlen("VmRSS:"), NULL, 10);
    fclose(status);
    return value;
}
static void write_maps(FILE *out) {
    FILE *maps = fopen("/proc/self/maps", "r");
    char line[SAMPLER_LINE_BYTES];
    fputs("MAPS\n", out);
    while (maps != NULL && fgets(line, sizeof(line), maps) != NULL)
        fputs(line, out);
    if (maps != NULL)
        fclose(maps);
    fputs("END\n", out);
}
/* Format: "THREADS n", n "tid start" lines, "MAPS", /proc/self/maps lines,
 * "END", "SAMPLES n", then n binary struct sample records. */
static void dump(const char *path) {
    atomic_store(&recording, 0);
    usleep(SAMPLER_QUIESCE_US);
    unsigned long count = atomic_load(&sample_count);
    if (count > SAMPLER_CAPACITY)
        count = SAMPLER_CAPACITY;
    FILE *out = fopen(path, "wb");
    if (out != NULL) {
        int named = atomic_load(&thread_count);
        if (named > SAMPLER_MAX_THREADS)
            named = SAMPLER_MAX_THREADS;
        fprintf(out, "THREADS %d\n", named);
        for (int i = 0; i < named; i++)
            fprintf(out, "%u %llx\n", threads[i].tid, (unsigned long long)threads[i].start);
        write_maps(out);
        fprintf(out, "SAMPLES %lu\n", count);
        if (samples != NULL)
            fwrite(samples, sizeof(struct sample), count, out);
        fclose(out);
    }
    atomic_store(&sample_count, 0);
    atomic_store(&recording, 1);
}
static void trim(const char *path) {
    long before = rss_kib();
    malloc_trim(0);
    long after = rss_kib();
    FILE *out = fopen(path, "w");
    if (out == NULL)
        return;
    fprintf(out, "%ld %ld\n", before, after);
    fclose(out);
}
static void malloc_report(const char *path) {
    FILE *out = fopen(path, "w");
    if (out == NULL)
        return;
    malloc_info(0, out);
    fclose(out);
}
static void run_command(char *line) {
    line[strcspn(line, "\n")] = 0;
    if (strncmp(line, "dump ", strlen("dump ")) == 0)
        dump(line + strlen("dump "));
    else if (strcmp(line, "reset") == 0)
        atomic_store(&sample_count, 0);
    else if (strncmp(line, "trim ", strlen("trim ")) == 0)
        trim(line + strlen("trim "));
    else if (strncmp(line, "mallinfo ", strlen("mallinfo ")) == 0)
        malloc_report(line + strlen("mallinfo "));
}
/* Reopen the FIFO after each writer closes it, forever. */
static void *control(void *context) {
    const char *fifo = context;
    for (;;) {
        FILE *commands = fopen(fifo, "r");
        if (commands == NULL)
            return NULL;
        char line[SAMPLER_LINE_BYTES];
        while (fgets(line, sizeof(line), commands) != NULL)
            run_command(line);
        fclose(commands);
    }
}
static void start_control(const char *fifo) {
    pthread_attr_t attributes;
    if (pthread_attr_init(&attributes) != 0)
        return;
    pthread_t thread;
    if (pthread_attr_setdetachstate(&attributes, PTHREAD_CREATE_DETACHED) == 0) {
        /* The control thread is not sampled: it bypasses the wrapper. */
        int code = original_create()(&thread, &attributes, control, (void *)fifo);
        (void)code;
    }
    pthread_attr_destroy(&attributes);
}
__attribute__((constructor)) static void start_sampler(void) {
    void *buffer = mmap(NULL, sizeof(struct sample) * SAMPLER_CAPACITY, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    samples = buffer == MAP_FAILED ? NULL : buffer;
    struct sigaction action = {.sa_sigaction = on_sample, .sa_flags = SA_SIGINFO | SA_RESTART};
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGPROF, &action, NULL) != 0)
        return;
    arm_thread(0);
    const char *fifo = getenv("SAMPLER_CTL");
    if (fifo != NULL)
        start_control(fifo);
}
