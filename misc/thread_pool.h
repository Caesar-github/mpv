#ifndef MPV_MP_THREAD_POOL_H
#define MPV_MP_THREAD_POOL_H

struct mp_thread_pool;

struct mp_thread_pool *mp_thread_pool_create(void *ta_parent, int threads);
void mp_thread_pool_queue(struct mp_thread_pool *pool, void (*fn)(void *ctx),
                          void *fn_ctx);

#endif
