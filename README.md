# Memory Pool Library for Embedded

[![CI](https://github.com/Yanujz/mempool/actions/workflows/ci.yml/badge.svg?branch=main)](https://github.com/Yanujz/mempool/actions/workflows/ci.yml?query=branch%3Amain)
[![Coverage](https://codecov.io/gh/Yanujz/mempool/branch/main/graph/badge.svg)](https://codecov.io/gh/Yanujz/mempool)
![Static Analysis](https://img.shields.io/badge/cppcheck-passing-brightgreen.svg)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)
![Language: C11](https://img.shields.io/badge/language-C11-blue.svg)
![Build: CMake](https://img.shields.io/badge/build-CMake-informational.svg)
![Tests: GoogleTest](https://img.shields.io/badge/tests-GoogleTest-success.svg)
![Sanitizers](https://img.shields.io/badge/sanitizers-ASan%20%7C%20UBSan%20%7C%20TSan-informational.svg)

Deterministic, zero-allocation memory pool library in C for embedded and
safety-related systems.  Allocation and deallocation run in **O(1)** time with
no hidden recursion, no heap usage, and no global state.  Version **0.5.0**.

---

## Table of Contents

1. [Key Properties](#key-properties)
2. [Quick Start](#quick-start)
3. [Complexity Guarantees](#complexity-guarantees)
4. [Feature Flags](#feature-flags)
5. [Porting Guide](#porting-guide)
6. [Safety Properties](#safety-properties)
7. [Multi-pool Manager](#multi-pool-manager)
8. [Building & Testing](#building--testing)
9. [Generating API Docs](#generating-api-docs)
10. [Safety-Oriented Process](#safety-oriented-process)

---

## Key Properties

| Property | Detail |
|----------|--------|
| Allocation | O(1) — free-list head pop |
| Deallocation | O(1) — free-list head push |
| Memory | Zero dynamic allocation; caller supplies all buffers |
| Fragmentation | None — fixed-size blocks |
| Reentrancy | User-supplied lock macros; zero overhead when single-threaded |
| ISR safety | Optional deferred-free ring buffer (`MEMPOOL_ENABLE_ISR_FREE`) |
| Portability | C11; no compiler extensions required; `__attribute__` macros unused in core |
| State size | Configurable `MEMPOOL_STATE_SIZE`; `_Static_assert` validates at compile time |

---

## Quick Start

```c
#include "mempool.h"

/* Size helpers: use MEMPOOL_POOL_BUFFER_SIZE for a conservative compile-time
 * upper bound, or call mempool_pool_buffer_size() at runtime for the exact size
 * for the active feature set. */
#define N_BLOCKS  32U
#define BLOCK_SZ  64U
#define ALIGN     8U

/* alignas(ALIGN) or __attribute__((aligned(ALIGN))) — your choice */
static uint8_t state_buf[MEMPOOL_STATE_SIZE]
    __attribute__((aligned(ALIGN)));
static uint8_t pool_buf[MEMPOOL_POOL_BUFFER_SIZE(BLOCK_SZ, N_BLOCKS, ALIGN)]
    __attribute__((aligned(ALIGN)));

mempool_t *pool;

mempool_error_t err = mempool_init(
    state_buf, sizeof state_buf,
    pool_buf,  sizeof pool_buf,
    BLOCK_SZ, ALIGN,
    &pool);

if (err != MEMPOOL_OK) { /* handle */ }

void *blk;
err = mempool_alloc(pool, &blk);   /* O(1) */
/* use blk … */
err = mempool_free(pool, blk);     /* O(1) */
```

See `examples/` for ISR-safe and multi-pool examples.

---

## Complexity Guarantees

All complexity claims are **worst-case** (not amortised), and apply regardless
of the number of blocks in the pool unless stated otherwise.

| Function | Complexity | Notes |
|----------|-----------|-------|
| `mempool_init` | O(N) | Free-list construction + bitmap/tag zeroing. One-time startup cost. |
| `mempool_alloc` | O(1) | Free-list head pop. See ISR caveat below. |
| `mempool_alloc_zero` | O(block_size) | O(1) alloc + `memset` over user area. |
| `mempool_free` | O(1) | Pointer range check + optional bitmap bit read/clear + free-list prepend. See POISON caveat. |
| `mempool_reset` | O(N) | `memset` over bitmap/tags + free-list rebuild. Not for hot paths. |
| `mempool_contains` | O(1) | Two pointer comparisons. |
| `mempool_is_initialized` | O(1) | Magic-word comparison. |
| `mempool_block_size` | O(1) | Struct field read. |
| `mempool_capacity` | O(1) | Struct field read. |
| `mempool_pool_buffer_size` | O(1) | Pure arithmetic; no loops. |
| `mempool_get_stats` | O(1) | Struct copy. |
| `mempool_reset_stats` | O(1) | Four field writes. |
| `mempool_walk` | O(N) | Scans all bitmap bytes. Lock held throughout. |
| `mempool_is_block_allocated` | O(1) | Range check + index shift + one bitmap byte read. |
| `mempool_drain_isr_queue` | O(K) | K ≤ `MEMPOOL_ISR_QUEUE_CAPACITY` (compile-time constant). |
| `mempool_free_from_isr` | O(1) | Pointer validation + ring-buffer enqueue under ISR lock. |
| `mempool_mgr_alloc` | O(P) | P = number of pools (≤ `MEMPOOL_MGR_MAX_POOLS`). |
| `mempool_mgr_free` | O(P) | Linear scan of pool address ranges. |

**ISR drain caveat** — when `MEMPOOL_ENABLE_ISR_FREE=1`, `mempool_alloc` drains
all pending ISR frees before allocating.  The drain loop runs at most
`MEMPOOL_ISR_QUEUE_CAPACITY` iterations (default 8).  Each iteration is O(1),
so `mempool_alloc` remains O(1) with that constant as the WCET multiplier.
When `MEMPOOL_ENABLE_POISON=1` each drain iteration also performs a `memset`
over the user area: worst-case WCET = `MEMPOOL_ISR_QUEUE_CAPACITY × block_size`
bytes written.

**POISON caveat** — when `MEMPOOL_ENABLE_POISON=1`, both `mempool_alloc` and
`mempool_free` execute `memset(user_area, pattern, block_size)`.  The
complexity is O(block_size), but block_size is a compile-time constant for any
given pool, so the WCET is deterministic.

### Proof that `mempool_is_block_allocated` is O(1)

```
mp_validate_block(pool, block):
  1. (uintptr_t)block vs pool->blocks_start/end   — 2 comparisons
  2. offset = block - blocks_start                 — 1 subtraction
  3. power-of-2 stride: offset & (stride-1) == 0  — 1 AND + 1 compare
     else: offset % block_size == 0               — 1 modulo + 1 compare

mp_block_idx(pool, block):
  power-of-2 stride: offset >> block_shift        — 1 right-shift
  else: offset / block_size                       — 1 division

mp_bitmap_is_set(pool, idx):
  bi  = idx >> 3                                  — 1 right-shift
  m   = 1 << (idx & 7)                           — 1 AND + 1 left-shift
  pool->bitmap[bi] & m                            — 1 load + 1 AND
```

Total: ≤ 11 arithmetic/logical operations + 1 memory load.
No loops. No data-structure traversal. No recursion. **O(1). QED.**

### Proof that `mempool_alloc` and `mempool_free` are O(1)

`mempool_alloc` (ISR_FREE disabled):
```
1. NULL checks                                    — 2 comparisons
2. Magic check                                    — 1 comparison
3. MEMPOOL_LOCK()                                 — platform-defined, O(1)
4. free_list == NULL check                        — 1 comparison
5. nd = free_list; free_list = nd->next           — 2 loads + 1 store
6. mp_bitmap_set: 1 load + 1 OR + 1 store        — 3 ops
7. canary write: 1 pointer add + 1 store         — 2 ops
8. stats update: 4 increments                     — 4 stores
9. *block = nd; MEMPOOL_UNLOCK()                  — 1 store
```
Total: ≤ 20 scalar ops + 1 platform lock. No loops. **O(1). QED.**

`mempool_free`:
```
1. NULL checks + magic check                      — 3 comparisons
2. mp_validate_block (see above)                  — ≤ 6 ops
3. MEMPOOL_LOCK()                                 — platform-defined
4. canary read + compare                          — 2 ops
5. mp_bitmap_is_set + mp_bitmap_clear             — 5 ops
6. memset (POISON)                                — O(block_size), constant
7. nd->next = free_list; free_list = nd           — 2 stores
8. stats update: 3 ops
9. MEMPOOL_UNLOCK()
```
Total: ≤ 25 scalar ops + 1 memset(block_size). No loops. **O(1) in N. QED.**

---

## Feature Flags

All features default to **OFF** except `STATS`, `DOUBLE_FREE_CHECK`, and
`STRERROR`.  Override per-flag at compile time:
`-DMEMPOOL_ENABLE_GUARD=1 -DMEMPOOL_ENABLE_POISON=1`

| Flag | Default | RAM cost (state) | Pool-buffer cost | Purpose |
|------|---------|-----------------|-----------------|---------|
| `MEMPOOL_ENABLE_STATS` | 1 | +40 B | 0 | Alloc/free counters, peak usage |
| `MEMPOOL_ENABLE_DOUBLE_FREE_CHECK` | 1 | +16 B | ⌈N/8⌉ bytes | Double-free bitmap |
| `MEMPOOL_ENABLE_STRERROR` | 1 | 0 (flash) | 0 | Human-readable error strings |
| `MEMPOOL_ENABLE_GUARD` | 0 | +4 B | +4 B/block | Post-canary overrun detection |
| `MEMPOOL_ENABLE_POISON` | 0 | 0 | 0 | Alloc/free memset fill |
| `MEMPOOL_ENABLE_OOM_HOOK` | 0 | +16 B | 0 | Out-of-memory callback |
| `MEMPOOL_ENABLE_ISR_FREE` | 0 | +8+8K B¹ | 0 | ISR deferred-free ring buffer |
| `MEMPOOL_ENABLE_TAGS` | 0 | +8 B | N × 4 B | Per-block 32-bit annotation |
| `MEMPOOL_ISR_QUEUE_CAPACITY` | 8 | — | — | Ring-buffer depth (≤ 255) |
| `MEMPOOL_STATE_SIZE` | 256 | — | — | State-buffer sizing constant |

¹ ISR state = 3 × `uint8_t` counters + `CAPACITY` × `sizeof(void*)` pointer array.

---

## Porting Guide

The library is written in C11 with no compiler extensions.  The only
integration points are four macros in `include/mempool_cfg.h`:

```c
MEMPOOL_LOCK()      /* enter task-level critical section  */
MEMPOOL_UNLOCK()    /* leave task-level critical section  */
MEMPOOL_ISR_LOCK()  /* enter ISR-level critical section   */
MEMPOOL_ISR_UNLOCK()/* leave ISR-level critical section   */
```

When undefined they expand to `((void)0)` — safe for single-threaded or
cooperative-multitasking environments where the caller guarantees serialised
access.

### FreeRTOS

```c
/* In mempool_cfg.h or your build system: */
#include "FreeRTOS.h"
#include "task.h"

#define MEMPOOL_LOCK()   taskENTER_CRITICAL()
#define MEMPOOL_UNLOCK() taskEXIT_CRITICAL()

/* taskENTER_CRITICAL already disables interrupts on Cortex-M, so ISR
 * lock can share the same implementation, OR use a separate
 * interrupt-disable for multi-core: */
#define MEMPOOL_ISR_LOCK()   taskENTER_CRITICAL_FROM_ISR()
#define MEMPOOL_ISR_UNLOCK() taskEXIT_CRITICAL_FROM_ISR(0)
```

### Zephyr RTOS

```c
#include <zephyr/kernel.h>

static struct k_spinlock mempool_spinlock;
static k_spinlock_key_t  mempool_key;

#define MEMPOOL_LOCK()    (mempool_key = k_spin_lock(&mempool_spinlock))
#define MEMPOOL_UNLOCK()  k_spin_unlock(&mempool_spinlock, mempool_key)
/* On single-core targets the same spinlock covers ISR context too. */
#define MEMPOOL_ISR_LOCK()    MEMPOOL_LOCK()
#define MEMPOOL_ISR_UNLOCK()  MEMPOOL_UNLOCK()
```

### Bare-metal Cortex-M (CMSIS)

```c
#include "cmsis_gcc.h"   /* or cmsis_armcc.h */

#define MEMPOOL_LOCK()   __disable_irq()
#define MEMPOOL_UNLOCK() __enable_irq()
/* ISR lock is the same on single-core bare-metal */
#define MEMPOOL_ISR_LOCK()   __disable_irq()
#define MEMPOOL_ISR_UNLOCK() __enable_irq()
```

### POSIX / host (testing)

```c
#include <pthread.h>
extern pthread_mutex_t mempool_mtx;

#define MEMPOOL_LOCK()   pthread_mutex_lock(&mempool_mtx)
#define MEMPOOL_UNLOCK() pthread_mutex_unlock(&mempool_mtx)
/* ISR free is not meaningful on POSIX; keep stubs. */
#define MEMPOOL_ISR_LOCK()   ((void)0)
#define MEMPOOL_ISR_UNLOCK() ((void)0)
```

### Buffer sizing

Use the compile-time macro for static arrays (conservative — assumes all
features ON):

```c
static uint8_t pool_buf[MEMPOOL_POOL_BUFFER_SIZE(64, 32, 8)];
```

Or the runtime function for the exact size under the active build flags:

```c
size_t sz = mempool_pool_buffer_size(64, 32, 8);
/* Allocate sz bytes aligned to 8 from your linker section or static storage */
```

---

## Safety Properties

| Property | Mechanism |
|----------|-----------|
| Buffer overrun detection | 32-bit post-canary validated on every `mempool_free` and ISR drain (`MEMPOOL_ENABLE_GUARD`) |
| Double-free detection | Allocation bitmap checked before and after free (`MEMPOOL_ENABLE_DOUBLE_FREE_CHECK`) |
| Use-after-free visibility | `0xDD` fill on free, `0xCD` fill on alloc (`MEMPOOL_ENABLE_POISON`) |
| Invalid-pointer rejection | Range + alignment check on every `free` and ISR enqueue |
| OOM notification | Optional callback hook (`MEMPOOL_ENABLE_OOM_HOOK`) |
| Uninitialized-handle rejection | 16-bit magic sentinel checked on every public entry point |
| ISR-safe free | Deferred ring buffer; no task-level lock in ISR context |
| State-buffer overflow | `_Static_assert(MEMPOOL_STATE_SIZE >= sizeof(struct mempool))` |
| ISR queue index overflow | `_Static_assert(MEMPOOL_ISR_QUEUE_CAPACITY <= 255)` |

### What is NOT protected

- **Mid-block overwrites** — the canary only catches writes that reach the
  canary word immediately past the user area.  Writes that stay within the
  user area after free are surfaced only by the poison fill (visible in a
  debugger), not by the allocator.
- **Multi-word overruns that skip the canary** — a write of more than
  `block_size + 4` bytes (GUARD ON) may skip the canary and land in the next
  block undetected.
- **Post-reset double-free** — after `mempool_reset()` the bitmap is cleared;
  a pointer held from before the reset is not detected as a double-free.
- **Manager-level thread safety** — `mempool_mgr` does not take a lock; protect
  it with an external mutex if used from multiple threads simultaneously.

---

## Multi-pool Manager

`mempool_mgr` routes an allocation to the smallest pool whose block size
satisfies the request:

```c
mempool_mgr_t mgr;
mempool_t *pools[3] = { p32, p256, p2k };
mempool_mgr_init(&mgr, pools, 3);  /* sorted internally */

void *buf; mempool_t *owner;
mempool_mgr_alloc(&mgr, 20,  &buf, &owner);   /* picks p32  */
mempool_mgr_alloc(&mgr, 100, &buf, &owner);   /* picks p256 */
mempool_mgr_free(&mgr, buf);                   /* auto-detects owner */
```

`mempool_mgr_alloc` is O(P) where P = number of pools.  `mempool_mgr_free` is
also O(P) (linear address-range scan).  Both are O(1) for fixed P.

---

## Building & Testing

```bash
mkdir build && cd build
cmake ..
make -j$(nproc)
ctest --output-on-failure
```

Optional CMake flags:

```bash
cmake -DMEMPOOL_ENABLE_SANITIZERS=ON   # ASan + UBSan + TSan
cmake -DMEMPOOL_ENABLE_COVERAGE=ON     # gcov/lcov coverage
cmake -DMEMPOOL_OPT_GUARD=ON           # enable guard canary in tests
cmake -DMEMPOOL_OPT_STATS=OFF          # disable stats in no-stats build
```

Test targets:

| Target | Description |
|--------|-------------|
| `mempool_gtest` | Core GoogleTest suite |
| `mempool_gtest_comprehensive` | Extended edge cases |
| `mempool_gtest_hardening` | All features ON (613 tests) |
| `mempool_gtest_mgr_nostats` | Manager with STATS=0 |
| `mempool_ctest` | C harness (no C++ runtime) |

---

## Generating API Docs

```bash
doxygen Doxyfile
open docs/doxygen/html/index.html
```

Requires [Doxygen](https://www.doxygen.nl/) ≥ 1.9.  The Doxyfile forces all
feature-guard macros open so every function appears in the generated HTML
regardless of the active build configuration.

---

## Safety-Oriented Process (Free Tooling)

* `docs/requirements.md` – functional & safety-related requirements.
* `docs/traceability.md` – high-level mapping from requirements to code and tests.
* `docs/safety/safety_manual.md` – integration guidelines and assumptions.
* `docs/safety/dev_plan.md` – suggested development & verification activities.
* `AUDIT.md` – 6-pass correctness audit with invariant proofs and threat model.

### Suggested Free Toolchain

| Activity | Tool |
|----------|------|
| Compiler warnings | GCC/Clang `-Wall -Wextra -Wpedantic -Wconversion` |
| Static analysis | `cppcheck`, `clang-tidy` |
| Dynamic analysis | `-fsanitize=address,undefined,thread` |
| Coverage | `gcov`/`lcov` or `llvm-cov` |
| API documentation | Doxygen (`doxygen Doxyfile`) |

## License
This project is licensed under the MIT License - see the [LICENSE](LICENSE) file for details.
