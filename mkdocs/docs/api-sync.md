# API Reference – Synchronization and Thread Safety

The core implementation is **not inherently thread-safe**. Instead, it exposes a
simple hook mechanism so callers can integrate with platform-specific
synchronization (e.g. mutexes, critical sections, or RTOS primitives).

This is optional: on single-threaded systems, you can ignore these hooks.

---

## Lock and Unlock Callbacks

The header defines function pointer types:

```c
typedef void (*mempool_lock_fn)(void *ctx);
typedef void (*mempool_unlock_fn)(void *ctx);
```

The `ctx` argument is an opaque pointer that is passed through from the pool to
the callback, typically pointing to a mutex or wrapper structure.

---

## `mempool_set_sync`

```c
mempool_error_t mempool_set_sync(mempool_t      *pool,
                                 mempool_lock_fn lock,
                                 mempool_unlock_fn unlock,
                                 void           *user_ctx);
```

### Parameters

- `pool` – Pool handle.
- `lock` – Function called before entering a critical section.
- `unlock` – Function called after leaving a critical section.
- `user_ctx` – Pointer passed to both callbacks as `ctx`.

### Behavior

- If both `lock` and `unlock` are non-`NULL`, synchronization is enabled:
  - `lock(user_ctx)` is called before modifying internal pool state.
  - `unlock(user_ctx)` is called afterwards.
- If either `lock` or `unlock` is `NULL`, synchronization is disabled.

### Returns

- `MEMPOOL_OK` on success.
- `MEMPOOL_ERR_NULL_PTR` if `pool` is `NULL`.
- `MEMPOOL_ERR_NOT_INITIALIZED` if the pool is not initialized.

---

## Example – Using `pthread_mutex_t`

```c
#include <pthread.h>
#include "mempool.h"

typedef struct {
    pthread_mutex_t mutex;
} pool_lock_ctx_t;

static void pool_lock(void *ctx)
{
    pool_lock_ctx_t *c = (pool_lock_ctx_t *)ctx;
    (void)pthread_mutex_lock(&c->mutex);
}

static void pool_unlock(void *ctx)
{
    pool_lock_ctx_t *c = (pool_lock_ctx_t *)ctx;
    (void)pthread_mutex_unlock(&c->mutex);
}

static uint8_t state_buf[MEMPOOL_STATE_SIZE];
static uint8_t pool_buf[4096U];

static pool_lock_ctx_t lock_ctx = { PTHREAD_MUTEX_INITIALIZER };

void init_thread_safe_pool(void)
{
    mempool_t *pool = NULL;

    (void)mempool_init(state_buf, sizeof(state_buf),
                       pool_buf,  sizeof(pool_buf),
                       64U, 8U, &pool);

    (void)mempool_set_sync(pool, pool_lock, pool_unlock, &lock_ctx);
}
```

All subsequent calls to `mempool_alloc`, `mempool_free`, `mempool_reset`,
and `mempool_get_stats` on this `pool` will be wrapped in `pool_lock` /
`pool_unlock` calls.

---

## Notes and Recommendations

- Synchronization is **per pool**. If you have multiple pools sharing the same
  low-level resource, use a shared context that locks all of them consistently.
- On very small embedded systems, `lock`/`unlock` can simply disable/enable
  interrupts or enter/exit a critical section.
- For maximum determinism, keep the callback bodies short and bounded; avoid
  calling unbounded or blocking APIs from inside lock/unlock.
