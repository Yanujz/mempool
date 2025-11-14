# MISRA-Friendly Mempool Library (Opaque, Two-Buffer Design)

This repository contains a small, deterministic memory pool library in C,
designed for embedded and safety-related systems.

The design uses:

- an **opaque** `mempool_t` state object stored in caller-provided memory,
- a separate caller-provided **pool buffer** for the actual blocks,
- no dynamic allocation and O(1) allocation/free.

The library is not itself "ASIL-certified", but the repository structure and
tooling are intended to support use in ISO 26262-style workflows using only
free tools.

## Highlights

- Opaque `mempool_t` state managed in caller-supplied buffer.
- Configurable `MEMPOOL_STATE_SIZE` with compile-time checks.
- Deterministic fixed-size block allocation and deallocation.
- Error codes for all failure modes (`MEMPOOL_ERR_*`).
- Statistics tracking (`mempool_stats_t`).
- Optional synchronization hooks (`mempool_set_sync`).
- C test harness + GoogleTest-based C++ tests (including multithreaded tests).

## Basic Usage

```c
#include "mempool.h"

#define POOL_BYTES 4096U

static uint8_t state_buf[MEMPOOL_STATE_SIZE] __attribute__((aligned(8)));
static uint8_t pool_buf[POOL_BYTES]          __attribute__((aligned(8)));

void init_my_pool(void)
{
    mempool_t *pool = NULL;

    if (mempool_state_size() > sizeof(state_buf)) {
        /* Configuration error: adjust MEMPOOL_STATE_SIZE or state_buf */
        /* handle error (assert, log, etc.) */
    }

    mempool_error_t err = mempool_init(
        state_buf, sizeof(state_buf),
        pool_buf, sizeof(pool_buf),
        64U,    /* block size */
        8U,     /* alignment (power of two) */
        &pool   /* out: pool handle */
    );

    if (err != MEMPOOL_OK) {
        /* handle error */
    }

    /* store pool somewhere, or pass it to other subsystems */
}
```

See `examples/` for more complete programs.

## Building

### Using CMake

```bash
mkdir build && cd build
cmake ..
make
ctest
```

This builds:

- `libmempool.a`
- `mempool_ctest` (C harness)
- `mempool_gtest` (GoogleTest suite)
- Examples: `example_basic`, `example_embedded`, `example_stress`

### Using Make

```bash
make           # builds lib + C test harness
make test      # runs C test harness
make examples  # builds example binaries
```

## Safety-Oriented Process (Free Tooling)

This repo contains documents and placeholders to help integrate the library
into a safety case:

- `docs/requirements.md` – functional & safety-related requirements.
- `docs/traceability.md` – high-level mapping from requirements to code and tests.
- `docs/safety/safety_manual.md` – integration guidelines and assumptions.
- `docs/safety/dev_plan.md` – suggested development & verification activities using free tools.
- `docs/safety/verification_report_template.md` – template to record results per release.

### Suggested Free Toolchain

- **Compiler warnings**: GCC/Clang with strict flags.
- **Static analysis**: `cppcheck`, `clang-tidy` (where available).
- **Dynamic analysis**: sanitizers (`-fsanitize=address,undefined,thread`) on host.
- **Coverage**: `gcov`/`lcov` or `llvm-cov` on host.

The actual commands and configurations are described in `docs/safety/dev_plan.md`.

## License

MIT. See `LICENSE`.
