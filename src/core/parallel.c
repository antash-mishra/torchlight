/* Fixed pthread workers dispatch disjoint ranges and retain no caller data
 * between runs. Allocation and thread lifecycle stay outside the hot path. */
#include "torchlight/parallel.h"
#include <pthread.h>
#include <stdbool.h>
#include <stdlib.h>
struct worker {
    tl_parallel *pool;
    pthread_t thread;
    size_t index;
    tl_status status;
};
struct tl_parallel {
    pthread_mutex_t mutex;
    pthread_cond_t ready, done;
    struct worker workers[PARALLEL_MAX_PARTICIPANTS - 1];
    size_t participants, started, remaining, count;
    uint64_t dispatch;
    tl_parallel_work work;
    void *context;
    bool mutex_ready, ready_ready, done_ready, active, stopping;
};
/* Initialized private primitives have one well-defined owner and lifecycle;
 * pthread errors here require a violation of the documented pool contract. */
static void lock_pool(tl_parallel *pool) {
    int code = pthread_mutex_lock(&pool->mutex);
    (void)code;
}
static void unlock_pool(tl_parallel *pool) {
    int code = pthread_mutex_unlock(&pool->mutex);
    (void)code;
}
static void signal_pool(pthread_cond_t *condition) {
    int code = pthread_cond_broadcast(condition);
    (void)code;
}
static void wait_pool(tl_parallel *pool, pthread_cond_t *condition) {
    int code = pthread_cond_wait(condition, &pool->mutex);
    (void)code;
}
/* Divide before multiplying so even count == SIZE_MAX cannot overflow. */
static tl_status work_range(tl_parallel *pool, size_t index) {
    size_t base = pool->count / pool->participants, extra = pool->count % pool->participants;
    size_t begin = index * base + (index < extra ? index : extra);
    size_t end = begin + base + (index < extra);
    return begin == end ? TL_OK : pool->work(pool->context, begin, end);
}
static void *worker_loop(void *context) {
    struct worker *worker = context;
    tl_parallel *pool = worker->pool;
    uint64_t dispatch = 0;
    lock_pool(pool);
    for (;;) {
        while (!pool->stopping && dispatch == pool->dispatch)
            wait_pool(pool, &pool->ready);
        if (pool->stopping)
            break;
        dispatch = pool->dispatch;
        unlock_pool(pool);
        tl_status status = work_range(pool, worker->index);
        lock_pool(pool);
        worker->status = status;
        if (--pool->remaining == 0)
            signal_pool(&pool->done);
    }
    unlock_pool(pool);
    return NULL;
}
static tl_status initialize_pool(tl_parallel *pool, size_t participants) {
    if (pthread_mutex_init(&pool->mutex, NULL) != 0)
        return TL_IO;
    pool->mutex_ready = true;
    if (pthread_cond_init(&pool->ready, NULL) != 0)
        return TL_IO;
    pool->ready_ready = true;
    if (pthread_cond_init(&pool->done, NULL) != 0)
        return TL_IO;
    pool->done_ready = true;
    pool->participants = participants;
    for (size_t i = 0; i + 1 < participants; i++) {
        pool->workers[i] = (struct worker){.pool = pool, .index = i + 1};
        if (pthread_create(&pool->workers[i].thread, NULL, worker_loop, &pool->workers[i]) != 0)
            return TL_IO;
        pool->started++;
    }
    return TL_OK;
}
tl_status parallel_create(size_t participants, tl_parallel **out) {
    if (out == NULL)
        return TL_INVALID;
    *out = NULL;
    if (participants == 0 || participants > PARALLEL_MAX_PARTICIPANTS)
        return TL_INVALID;
    tl_parallel *pool = calloc(1, sizeof(*pool));
    if (pool == NULL)
        return TL_NOMEM;
    tl_status status = initialize_pool(pool, participants);
    if (status != TL_OK) {
        parallel_destroy(pool);
        return status;
    }
    *out = pool;
    return TL_OK;
}
void parallel_destroy(tl_parallel *pool) {
    if (pool == NULL)
        return;
    if (pool->mutex_ready) {
        lock_pool(pool);
        pool->stopping = true;
        if (pool->ready_ready)
            signal_pool(&pool->ready);
        unlock_pool(pool);
    }
    for (size_t i = 0; i < pool->started; i++) {
        int code = pthread_join(pool->workers[i].thread, NULL);
        (void)code;
    }
    if (pool->done_ready) {
        int code = pthread_cond_destroy(&pool->done);
        (void)code;
    }
    if (pool->ready_ready) {
        int code = pthread_cond_destroy(&pool->ready);
        (void)code;
    }
    if (pool->mutex_ready) {
        int code = pthread_mutex_destroy(&pool->mutex);
        (void)code;
    }
    free(pool);
}
tl_status parallel_run(tl_parallel *pool, size_t count, tl_parallel_work work, void *context) {
    if (pool == NULL || work == NULL)
        return TL_INVALID;
    lock_pool(pool);
    if (pool->active || pool->stopping) {
        unlock_pool(pool);
        return TL_STATE;
    }
    pool->active = true;
    pool->work = work;
    pool->context = context;
    pool->count = count;
    pool->remaining = pool->started;
    pool->dispatch++;
    signal_pool(&pool->ready);
    unlock_pool(pool);
    tl_status status = work_range(pool, 0);
    lock_pool(pool);
    while (pool->remaining != 0)
        wait_pool(pool, &pool->done);
    for (size_t i = 0; i < pool->started && status == TL_OK; i++)
        status = pool->workers[i].status;
    pool->active = false;
    pool->work = NULL;
    pool->context = NULL;
    unlock_pool(pool);
    return status;
}
