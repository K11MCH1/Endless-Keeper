/******************************************************************************/
// Free implementation of Bullfrog's Dungeon Keeper strategy game.
/******************************************************************************/
/** @file kfx_jobs.c
 *     A parallel-for on job threads (enkiTS, deps/enkiTS).
 */
/******************************************************************************/
#include "kfx_jobs.h"
#include "kfx/profiling/KfxProfilingC.h"
#include <stdlib.h>
#include "../deps/enkiTS/src/TaskScheduler_c.h"

struct KfxJobCall {
    kfx_job_fn fn;
    void *args;
};

static enkiTaskScheduler *job_ets;
static enkiTaskSet *job_task;
static int job_threads = -1;

static void job_trampoline(uint32_t start, uint32_t end, uint32_t thread, void *args)
{
    const struct KfxJobCall *c = (const struct KfxJobCall *)args;
#ifdef TRACY_ENABLE
    static __thread int named;
    if (!named && (thread != 0)) /* 0: the thread that waits (the main one) */
    {
        named = 1;
        TracyCSetThreadName("job thread");
    }
#endif
    KFX_C_ZONE_BEGIN(z_job, "job range");
    c->fn(start, end, thread, c->args);
    KFX_C_ZONE_END(z_job);
}

int kfx_job_threads(void)
{
    if (job_threads >= 0)
        return job_threads;
    const char *v = getenv("KFX_NET_THREADS");
    const int want = (v != NULL) ? atoi(v) : 0;
    if ((v != NULL) && (want <= 1))
    {
        job_threads = 1;
        return job_threads;
    }
    job_ets = enkiNewTaskScheduler();
    if (job_ets == NULL)
    {
        job_threads = 1;
        return job_threads;
    }
    struct enkiTaskSchedulerConfig cfg = enkiGetTaskSchedulerConfig(job_ets);
    uint32_t n = cfg.numTaskThreadsToCreate; /* the hardware's threads less one */
    if (want > 1)
        n = (uint32_t)(want - 1);
    else if (n > 7)
        n = 7; /* the work is mostly memory compares: more threads don't pay */
    if (n < 1)
        n = 1;
    cfg.numTaskThreadsToCreate = n;
    enkiInitTaskSchedulerWithConfig(job_ets, cfg);
    job_task = enkiCreateTaskSet(job_ets, job_trampoline);
    job_threads = (job_task != NULL) ? (int)enkiGetNumTaskThreads(job_ets) : 1;
    return job_threads;
}

void kfx_parallel_for(uint32_t count, uint32_t min_range, kfx_job_fn fn, void *args)
{
    if (count == 0)
        return;
    if ((kfx_job_threads() <= 1) || (job_task == NULL) || (count <= min_range))
    {
        fn(0, count, 0, args);
        return;
    }
    struct KfxJobCall c = {fn, args};
    enkiSetMinRangeTaskSet(job_task, (min_range > 0) ? min_range : 1);
    enkiAddTaskSetArgs(job_ets, job_task, &c, count);
    enkiWaitForTaskSet(job_ets, job_task);
}
