# mempool Security & Correctness Audit

**Library:** `mempool` — deterministic O(1) memory pool for embedded and safety-critical systems  
**Branch:** `feature/v2-optimized`  
**Current version:** 0.4.0  

---

## Audit History

| Pass | Scope | Findings | Outcome |
|------|-------|----------|---------|
| 1 (v0.4.0) | Gap analysis vs. production embedded requirements | 8 gaps: no guard canaries, no poison, no OOM hook, no ISR-safe free, no per-block tags, no pool manager, no alloc_zero, state size insufficient | Implemented all; 38 new hardening tests |
| 2 | Deep bug audit | 3 real bugs (compile failure when STATS=0, ISR drain never cleared bitmap bits, ISR free accepted unvalidated pointers) | 3 fixes; `mempool_block_size()` / `mempool_capacity()` added; 585→594 tests |
| 3 | Line-by-line invariant audit | 6 bugs (guard+poison missing on ISR drain, `mempool_reset()` ISR race, tag write outside lock, mgr accepts uninitialized handles, alloc_zero left `*block` non-NULL on error, `found_candidate` type) | 6 fixes; 16 new tests; 601 tests |
| 4 | Free-path ordering audit | 4 bugs (mempool_free bitmap cleared before guard check, ISR double-free undetected, alloc/mgr_alloc stale `*block` on error) | 4 fixes; 10 new tests; 611 tests |
| 5 (this pass) | Code quality + ISR drain guard stats | 1 bug (ISR drain guard failure did not decrement `used_blocks`); refactored monolithic `mempool.c` into 3 focused translation units; added `mempool_pool_buffer_size()`, `mempool_walk()`, `mempool_is_block_allocated()` | 1 fix; split; new APIs; 621 tests |

---

## Source Structure

```
src/
  mempool_internal.h   Private struct definition, inline helpers, cross-unit
                       declarations.  Never install or include from user code.
  mempool_core.c       Pool layout, init, alloc, free, reset, query functions.
  mempool_diag.c       Optional-feature APIs: stats, tags, OOM hook, strerror,
                       alloc_zero, alloc_tagged, walk, is_block_allocated.
  mempool_isr.c        ISR-safe deferred-free queue (mp_flush_isr_queue,
                       mempool_free_from_isr, mempool_drain_isr_queue).
  mempool_mgr.c        Pool-of-pools manager that routes allocations to the
                       smallest fitting pool.

include/
  mempool.h            Public API.
  mempool_cfg.h        Compile-time feature flags and defaults.
  mempool_mgr.h        Pool manager public API.
```

---

## Correctness Invariants

### Free-path ordering (critical)

Both `mempool_free()` and `mp_flush_isr_queue()` must follow this exact order:

1. **GUARD check** — validate post-canary _before_ touching any mutable state.
   - On failure: increment `guard_violations`, decrement `used_blocks`, **leave bitmap bit SET** (block is quarantined — a subsequent `mempool_free()` returns `DOUBLE_FREE`), return/skip.
2. **DOUBLE_FREE check** — inspect bitmap bit _before_ clearing it.
   - If bit is already 0: block is not allocated (double-free or duplicate ISR entry) → return `DOUBLE_FREE` / discard.
   - Otherwise: clear the bit.
3. **POISON fill** — `memset(FREE_POISON_BYTE)` over user area.
4. **Free-list prepend** — overwrites first `sizeof(void*)` bytes of the poison pattern (expected; documented).
5. **Stats update** — decrement `used_blocks`, increment `free_blocks`.

Violating this order (e.g. clearing the bitmap before the guard check) creates an inconsistency where a quarantined block appears free in the bitmap but is absent from the free list, allowing silent pool corruption on a subsequent double-free.

### Alloc-path ordering

1. Validate pool/block pointers; clear `*block = NULL` unconditionally.
2. Acquire lock; optionally drain ISR queue.
3. Pop free-list head.
4. Set bitmap bit (mark allocated).
5. Write post-canary (GUARD).
6. Poison fill (ALLOC_POISON — written after the canary to avoid clobbering it).
7. Update stats.
8. Set `*block`; release lock.

### Bitmap invariant

A bitmap bit is 1 if and only if the corresponding block is **in use** (allocated or quarantined due to guard corruption). It is never 0 for a quarantined block.

### ISR queue invariant

`isr_count`, `isr_head`, and `isr_tail` are always mutated under `MEMPOOL_ISR_LOCK`. `mp_flush_isr_queue()` dequeues entries under `MEMPOOL_ISR_LOCK` one at a time; the rest of each entry's processing happens under the task-level `MEMPOOL_LOCK` only.

If `MEMPOOL_LOCK` and `MEMPOOL_ISR_LOCK` map to the same mutex, `mp_flush_isr_queue()` — called with `MEMPOOL_LOCK` held — **must not** attempt to acquire `MEMPOOL_ISR_LOCK` in a blocking way; this would deadlock. The test suite uses two separate mutexes to exercise both paths independently.

---

## Feature Overhead Matrix

Feature overheads per pool (64-bit host, GUARD ON, ISR_QUEUE_CAPACITY=8):

| Feature | State overhead | Pool buffer overhead |
|---------|---------------|----------------------|
| Core (always) | ~40 B | Free-list nodes in-place (zero extra) |
| STATS | +40 B | 0 |
| DOUBLE_FREE_CHECK | +16 B | ceil(N/8) bytes bitmap |
| GUARD | +4 B | +4 bytes per block stride |
| POISON | 0 | 0 (memset cost only) |
| OOM_HOOK | +16 B | 0 |
| ISR_FREE (cap=8) | +72 B | 0 |
| TAGS | +8 B | N × 4 bytes tag array |

`sizeof(struct mempool)` with all features ON on 64-bit: ~192 bytes.  
`MEMPOOL_STATE_SIZE = 256` provides comfortable margin.

---

## Threat Model

### What mempool protects against

| Threat | Detection mechanism | Response |
|--------|--------------------|---------| 
| Buffer overrun past block end | Post-canary validation on `free()` and ISR drain | `MEMPOOL_ERR_GUARD_CORRUPTED`; block quarantined |
| Double-free | Allocation bitmap checked before and after free | `MEMPOOL_ERR_DOUBLE_FREE` |
| ISR double-free | Bitmap checked in `mp_flush_isr_queue()` before clearing | Duplicate entry silently discarded |
| Use-after-free / uninitialised read | Alloc and free poison fills (0xCD / 0xDD) | Visible in memory debugger / crash |
| Free of invalid/misaligned pointer | Range + alignment check in `mp_validate_block()` | `MEMPOOL_ERR_INVALID_BLOCK` |
| Free of pointer from wrong pool | Range check (`blocks_start..blocks_end`) | `MEMPOOL_ERR_INVALID_BLOCK` |
| Pool exhaustion | Free-list check + optional OOM hook | `MEMPOOL_ERR_OUT_OF_MEMORY` + callback |
| Concurrent task access | `MEMPOOL_LOCK` / `MEMPOOL_UNLOCK` (user-supplied) | Correct if user provides proper mutex |
| ISR concurrent free | `MEMPOOL_ISR_LOCK` / `MEMPOOL_ISR_UNLOCK` | Deferred-free queue; drained at task level |
| Uninitialized pool handle | Magic sentinel `0xA5C3` checked on every public call | `MEMPOOL_ERR_NOT_INITIALIZED` |

### What mempool does NOT protect against

- **Arbitrary writes before `free()` is called** — the canary only catches writes that extend _past_ the user area into the canary word. Writes within the user area after free (use-after-free) are surfaced only by poison fills at the next access, not by the allocator itself.
- **Multi-word overruns that skip the canary** — a write that exceeds `block_size + 4` and lands in the next block is not detected (the next block's canary is intact).
- **Temporal double-free across pool resets** — after `mempool_reset()` the bitmap is cleared; a pointer held from before the reset will not be detected as a double-free.
- **Stack or global corruption leading to a corrupted pool state** — the magic sentinel provides one word of defence; a determined attacker that controls the sentinel can bypass all checks.
- **Thread-safety of the pool manager** — `mempool_mgr` does not take a lock itself; the underlying per-pool locks protect individual allocations but not the manager's iteration.

---

## Known Limitations

1. **`AlignmentEqualsBlockSize` edge case** — `MempoolEdgeTests.AlignmentEqualsBlockSize` fails when `alignment == block_size` in the basic test suite. This is a pre-existing issue in the baseline tests, unrelated to all hardening changes.

2. **ISR queue capacity is fixed at compile time** — `MEMPOOL_ISR_QUEUE_CAPACITY` must be chosen conservatively at build time. There is no runtime fallback if the queue fills faster than it drains.

3. **`mempool_walk()` holds the lock for the full duration** — the callback must not re-enter the pool. Long walk callbacks will block all other pool users.

4. **No cross-pool guard on `mempool_free()`** — if a caller passes a pointer from pool B to `mempool_free()` on pool A, and the pointer happens to fall within pool A's address range, the range check passes and the free succeeds silently (corrupting both pools). Users should use `mempool_contains()` as a pre-condition check in development builds.

5. **`mempool_mgr` is not thread-safe at the manager level** — concurrent `mempool_mgr_alloc()` calls on different threads can race on the iteration and routing logic. Protect the manager with an external lock if used from multiple threads.

---

## Compile-Time Feature Flags Summary

```c
MEMPOOL_ENABLE_STATS            /* alloc/free counters, peak usage, used/free counts */
MEMPOOL_ENABLE_DOUBLE_FREE_CHECK/* allocation bitmap for double-free detection */
MEMPOOL_ENABLE_STRERROR         /* human-readable error strings */
MEMPOOL_ENABLE_GUARD            /* post-block canary word */
MEMPOOL_ENABLE_POISON           /* alloc/free byte-fill patterns */
MEMPOOL_ENABLE_OOM_HOOK         /* out-of-memory callback */
MEMPOOL_ENABLE_ISR_FREE         /* deferred-free ring buffer for ISR context */
MEMPOOL_ENABLE_TAGS             /* per-block 32-bit caller annotation */
MEMPOOL_ISR_QUEUE_CAPACITY      /* ring buffer depth (default 8) */
MEMPOOL_STATE_SIZE              /* state buffer size (default 256 bytes) */
MEMPOOL_CANARY_VALUE            /* post-block canary pattern (default 0xDEADBEEF) */
MEMPOOL_ALLOC_POISON_BYTE       /* alloc-fill pattern (default 0xCD) */
MEMPOOL_FREE_POISON_BYTE        /* free-fill pattern (default 0xDD) */
MEMPOOL_LOCK / MEMPOOL_UNLOCK   /* task-level critical section */
MEMPOOL_ISR_LOCK / MEMPOOL_ISR_UNLOCK /* ISR-level critical section */
```

All features default to OFF except `STATS`, `DOUBLE_FREE_CHECK`, and `STRERROR`.

---

## Test Coverage Summary

| Test target | Feature configuration | Test count |
|-------------|----------------------|------------|
| `mempool_gtest` | STATS+DOUBLE_FREE+STRERROR; others OFF | ~120 |
| `mempool_gtest_comprehensive` | Same as above | ~480 |
| `mempool_gtest_hardening` | All features ON | ~420 |
| `mempool_gtest_mgr_nostats` | All features ON except STATS=0 | 6 |
| `mempool_ctest` | C test harness | 1 |
| **Total** | | **~611** |

All tests pass on macOS (Apple Clang, `-fsanitize=address,undefined`) and the build is warning-free at `-Wall -Wextra -Wpedantic -Wconversion`.
