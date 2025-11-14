# Memory Pool Library

A production-grade, platform-agnostic memory pool (buffer pool) implementation
in C. Designed for embedded systems, real-time applications, and any
environment requiring deterministic memory allocation.

This version tightens correctness and robustness:

- Struct initialization is deterministic (no reliance on prior memory state)
- **Double-free detection is implemented with a bitmap (1 bit per block)**
- Optional thread-safety via user-supplied lock/unlock callbacks
- GoogleTest suite with multithreaded stress tests

## Features

- **C11, MISRA-friendly style** (no dynamic allocation inside the library)
- **Zero Dynamic Allocation in Core**: user supplies the backing buffer
- **O(1) Operations**: constant-time allocation and deallocation
- **Platform Agnostic**: no dependency on any particular threading API
- **Optional Thread Safety**: `mempool_set_sync` to plug in your mutexes
- **Robust Error Handling**: error codes for all failure modes
- **Double-Free Detection**: per-block bitmap validation
- **Statistics Tracking**: usage, peak allocation, operation counts
- **Tests & Examples**: C minimal harness + C++ GoogleTest suite

## Directory Structure

```text
mempool/
├── include/
│   └── mempool.h          # Public API
├── src/
│   └── mempool.c          # Implementation (C)
├── tests/
│   ├── test_mempool.c     # Minimal C test harness
│   ├── gtest_mempool.cpp  # GoogleTest C++ suite (incl. multithreaded tests)
│   └── test_runner.c      # Placeholder for embedded runners
├── examples/
│   ├── basic_usage.c
│   ├── embedded_example.c
│   └── stress_test.c
├── docs/
│   └── API.md
├── Makefile               # C-only build + basic tests/examples
├── CMakeLists.txt         # Full build with GoogleTest
├── README.md
├── LICENSE
└── CHANGELOG.md
```

## Quick Start

### Using CMake (recommended, with GoogleTest)

```bash
mkdir build && cd build
cmake ..
make
ctest       # runs both mempool_gtest and mempool_ctest
```

CMake will automatically fetch GoogleTest and build the C++ test suite.

### Using Make (C-only, no GoogleTest)

```bash
make              # builds libmempool.a and mempool_ctest
make test         # runs the minimal C test harness
make examples     # builds example_* binaries
```

## Basic Usage

```c
#include "mempool.h"

// Allocate buffer (must be aligned appropriately)
static uint8_t buffer[4096] __attribute__((aligned(8)));
mempool_t pool;

mempool_error_t err =
    mempool_init(&pool, buffer, sizeof(buffer), 64U, 8U);
if (err != MEMPOOL_OK) {
    // handle error
}

// Allocate block
void *block = NULL;
err = mempool_alloc(&pool, &block);
if (err == MEMPOOL_OK) {
    // Use block...

    // Free it again
    err = mempool_free(&pool, block);
}
```

## Thread-Safety (Platform Agnostic)

The core library does **not** depend on any specific threading API. Instead,
you configure per-pool synchronization via user callbacks:

```c
#include "mempool.h"
#include <pthread.h>

typedef struct {
    pthread_mutex_t mtx;
} pool_mutex_ctx_t;

static void pool_lock(void *ctx) {
    pool_mutex_ctx_t *c = (pool_mutex_ctx_t *)ctx;
    (void)pthread_mutex_lock(&c->mtx);
}

static void pool_unlock(void *ctx) {
    pool_mutex_ctx_t *c = (pool_mutex_ctx_t *)ctx;
    (void)pthread_mutex_unlock(&c->mtx);
}

void init_thread_safe_pool(void)
{
    static uint8_t buffer[4096] __attribute__((aligned(8)));
    static mempool_t pool;
    static pool_mutex_ctx_t ctx = { PTHREAD_MUTEX_INITIALIZER };

    mempool_init(&pool, buffer, sizeof(buffer), 64U, 8U);
    (void)mempool_set_sync(&pool, pool_lock, pool_unlock, &ctx);

    /* Now 'pool' is safe to use concurrently via mempool_alloc/free. */
}
```

In the C++ GoogleTest suite, this pattern is used with `std::mutex` and
`std::thread`, without changing the C library.

> **Note:** Do not modify synchronization configuration while other threads
> are using the pool. Call `mempool_set_sync` once after `mempool_init`
> and before publishing the pool to other threads.

## API Overview

See [`docs/API.md`](docs/API.md) for full details. Key functions:

- `mempool_init` – initialize pool with pre-allocated buffer
- `mempool_alloc` – allocate a fixed-size block
- `mempool_free` – free a block back to the pool (with double-free detection)
- `mempool_get_stats` – query usage statistics
- `mempool_reset` – restore pool to initial state
- `mempool_contains` – check if a pointer belongs to the pool
- `mempool_strerror` – translate error codes to messages
- `mempool_set_sync` – configure optional synchronization callbacks

## Error Codes

- `MEMPOOL_OK` - Success  
- `MEMPOOL_ERR_NULL_PTR` - Null pointer argument  
- `MEMPOOL_ERR_INVALID_SIZE` - Invalid size parameter  
- `MEMPOOL_ERR_OUT_OF_MEMORY` - Pool exhausted  
- `MEMPOOL_ERR_INVALID_BLOCK` - Block doesn't belong to pool (range/alignment)  
- `MEMPOOL_ERR_ALIGNMENT` - Alignment error  
- `MEMPOOL_ERR_DOUBLE_FREE` - Block is already free (or never allocated)  
- `MEMPOOL_ERR_NOT_INITIALIZED` - Pool not initialized  

## Testing

### GoogleTest Suite

`tests/gtest_mempool.cpp` includes:

- Initialization and validation tests
- Allocation/deallocation correctness
- Reset semantics
- Double-free and invalid pointer detection
- **Multithreaded stress tests** using `std::thread` and `std::mutex` via
  `mempool_set_sync`

Run with:

```bash
mkdir build && cd build
cmake ..
make
ctest
```

### Minimal C Harness

For environments without C++ or GoogleTest:

```bash
make
make test    # runs mempool_ctest
```

The C harness covers basic init/alloc/free and verifies that double-free
returns `MEMPOOL_ERR_DOUBLE_FREE`.

## MISRA / Safety Notes

The core is written in a conservative C11 subset intended to be friendly to
MISRA-C style guidelines (no dynamic allocation, no recursion, bounded loops).
However, **formal MISRA compliance still requires running a static analysis
tool (e.g., PC-lint, Coverity) and maintaining rule justifications**.

## License

MIT License – see [`LICENSE`](LICENSE).

## Contributing

Contributions welcome! Please ensure:

- No dynamic allocation is added to the core library
- All tests (C and GoogleTest) pass
- New features include tests
- Documentation is updated
