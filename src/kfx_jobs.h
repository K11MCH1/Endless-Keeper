/******************************************************************************/
// Free implementation of Bullfrog's Dungeon Keeper strategy game.
/******************************************************************************/
/** @file kfx_jobs.h
 *     A parallel-for on job threads (enkiTS), for the thin-client server's per-turn work.
 */
/******************************************************************************/
#ifndef DK_KFX_JOBS_H
#define DK_KFX_JOBS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*kfx_job_fn)(uint32_t start, uint32_t end, uint32_t thread, void *args);

/** Threads that run jobs, the caller included (thread numbers given to a job are below this); 1: no job threads.
 *  KFX_NET_THREADS=n sets it (1: everything on the calling thread); otherwise the cores less one, at most 8. */
int kfx_job_threads(void);
/** Runs fn over [0, count) in ranges of at least min_range, on the job threads and the caller; returns when all is
 *  done. Only from the main thread. */
void kfx_parallel_for(uint32_t count, uint32_t min_range, kfx_job_fn fn, void *args);

#ifdef __cplusplus
}
#endif
#endif
