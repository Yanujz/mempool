# Getting Started

This page shows how to integrate `mempool` into a project and use it in a minimal way.

## Requirements

- C11-capable compiler (GCC, Clang, or similar)
- Basic C standard library (`stddef.h`, `stdint.h`, `stdbool.h`)
- No OS or dynamic memory required

Optional but recommended for host development:

- CMake (for the reference build)
- GoogleTest (fetched automatically by CMake for the C++ tests)
- cppcheck, sanitizers, and coverage tools

---

## Basic Integration Model

The library does **not** allocate memory internally.

Instead, the caller provides:

1. A **state buffer** to hold the opaque `mempool_t` state.
2. A **pool buffer** to hold the fixed-size blocks.

At runtime you:

1. Call `mempool_state_size()` to learn how many bytes the state needs.
2. Allocate a state buffer (static array, global, or from another allocator).
3. Allocate a pool buffer for the actual blocks.
4. Call `mempool_init(...)` to initialize the pool and obtain a `mempool_t*` handle.
5. Use `mempool_alloc` / `mempool_free` to manage blocks.

---

## Minimal Example

```c
#include "mempool.h"

#define POOL_BYTES 4096U

/* Caller-provided storage */
static uint8_t state_buf[MEMPOOL_STATE_SIZE] __attribute__((aligned(8)));
static uint8_t pool_buf[POOL_BYTES]          __attribute__((aligned(8)));

void init_my_pool(void)
{
    mempool_t *pool = NULL;

    /* Sanity check: make sure our state buffer is large enough */
    if (mempool_state_size() > sizeof(state_buf)) {
        /* Configuration error: adjust MEMPOOL_STATE_SIZE or state_buf size */
        /* handle error (assert, log, etc.) */
        return;
    }

    mempool_error_t err = mempool_init(
        state_buf, sizeof(state_buf),   /* state buffer */
        pool_buf,  sizeof(pool_buf),    /* pool buffer */
        64U,                            /* block size */
        8U,                             /* alignment (power of two) */
        &pool                           /* out: pool handle */
    );

    if (err != MEMPOOL_OK) {
        /* handle error */
        return;
    }

    /* Allocate a block */
    void *block = NULL;
    err = mempool_alloc(pool, &block);
    if (err == MEMPOOL_OK) {
        /* Use the block... */

        /* Return it to the pool */
        (void)mempool_free(pool, block);
    }
}
```

---

## State Buffer Sizing

The header exposes:

```c
size_t mempool_state_size(void);
```

At compile time you typically define:

```c
#ifndef MEMPOOL_STATE_SIZE
#define MEMPOOL_STATE_SIZE 128U /* or larger */
#endif
```

Then allocate a buffer of that size:

```c
static uint8_t state_buf[MEMPOOL_STATE_SIZE];
```

At runtime, `mempool_state_size()` returns the actual bytes required by the implementation.
You **must** check that `mempool_state_size() <= MEMPOOL_STATE_SIZE` and treat a mismatch as
a configuration error.

---

## Next Steps

- See [Building](building.md) to compile and run tests.
- See [API Reference](api-core.md) for details on functions and error handling.
- See [Examples](examples-basic.md) for more concrete usage patterns.
