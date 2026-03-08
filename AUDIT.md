# mempool Security & Correctness Audit

**Library:** `mempool` — deterministic O(1) memory pool for embedded and safety-critical systems  
**Branch:** `feature/v2-optimized`  
**Current version:** 0.5.0  

---

## Audit History

| Pass | Scope | Findings | Outcome |
|------|-------|----------|---------|
| 1 (v0.4.0) | Gap analysis vs. production embedded requirements | 8 gaps: no guard canaries, no poison, no OOM hook, no ISR-safe free, no per-block tags, no pool manager, no alloc_zero, state size insufficient | Implemented all; 38 new hardening tests |
| 2 | Deep bug audit | 3 real bugs (compile failure when STATS=0, ISR drain never cleared bitmap bits, ISR free accepted unvalidated pointers) | 3 fixes; `mempool_block_size()` / `mempool_capacity()` added; 585→594 tests |
| 3 | Line-by-line invariant audit | 6 bugs (guard+poison missing on ISR drain, `mempool_reset()` ISR race, tag write outside lock, mgr accepts uninitialized handles, alloc_zero left `*block` non-NULL on error, `found_candidate` type) | 6 fixes; 16 new tests; 601 tests |
| 4 | Free-path ordering audit | 4 bugs (mempool_free bitmap cleared before guard check, ISR double-free undetected, alloc/mgr_alloc stale `*block` on error) | 4 fixes; 10 new tests; 611 tests |
| 5 | Code quality + ISR drain guard stats | 1 bug (ISR drain guard failure did not decrement `used_blocks`); refactored monolithic `mempool.c` into 3 focused translation units; added `mempool_pool_buffer_size()`, `mempool_walk()`, `mempool_is_block_allocated()` | 1 fix; split; new APIs; 611 tests |
| 6 (this pass) | Cross-unit deduplication, type-safety, tag lifetime, error propagation, documentation | 5 bugs (see below); Doxyfile; README overhaul with porting guide + O(1) proofs | 5 fixes; 2 new tests; 613 tests |

---

## Pass 6 Bug Findings

### Bug 6.1 — ISR queue index overflow (type-safety)
**File:** `src/mempool_internal.h` (new `_Static_assert`)  
**Severity:** High (silent data corruption if `MEMPOOL_ISR_QUEUE_CAPACITY > 255`)

`isr_head`, `isr_tail`, and `isr_count` are declared as `uint8_t`.  When
`MEMPOOL_ISR_QUEUE_CAPACITY` is set to a value greater than 255, the modulo
ring-buffer arithmetic silently wraps, producing out-of-bounds array indices
and arbitrary memory reads/writes in the ISR queue.

**Fix:** Added `_Static_assert(MEMPOOL_ISR_QUEUE_CAPACITY <= 255U, ...)` inside the
`#if MEMPOOL_ENABLE_ISR_FREE` guard so the compiler catches misconfiguration at
build time, before any runtime damage can occur.

---

### Bug 6.2 — Code duplication in `mempool_free_from_isr`
**File:** `src/mempool_isr.c`  
**Severity:** Maintenance / latent correctness risk

`mempool_free_from_isr` contained 15 lines of block range + alignment
validation code that was a verbatim copy of `mp_validate_block()` from
`mempool_internal.h`.  Two separate copies of the same logic diverge
over time; any future fix to `mp_validate_block` would need to be applied
manually to this second copy as well.

**Fix:** Replaced the duplicated code with a single call to
`mp_validate_block(pool, block)`.  The helper is `static inline` and reads
only immutable pool geometry, making it safe to call from ISR context.

---

### Bug 6.3 — Raw bitmap operations in `mp_flush_isr_queue`
**File:** `src/mempool_isr.c`  
**Severity:** Maintenance (bounds-check duplication)

`mp_flush_isr_queue` computed the bitmap byte index and bitmask inline
instead of calling the `mp_bitmap_is_set` / `mp_bitmap_clear` helpers.
The helpers already contain bounds-checking (`bi < pool->bitmap_bytes`);
the inline code duplicated that guard.

**Fix:** Replaced the inline computation with `mp_bitmap_is_set(pool, idx)` and
`mp_bitmap_clear(pool, idx)`.

---

### Bug 6.4 — Stale tag visible after reallocation
**File:** `src/mempool_core.c` (`mempool_free`)  
**Severity:** Medium (incorrect application-level state)

When `MEMPOOL_ENABLE_TAGS=1`, `mempool_free` did not clear the per-block tag.
After freeing a tagged block and reallocating it (typical LIFO free-list
behaviour), `mempool_get_block_tag` on the new owner returned the tag value
written by the previous owner.  In safety-critical code that uses tags to
track block ownership (e.g. task ID or buffer descriptor), this silently
associated a fresh block with the wrong owner.

**Fix:** Added a `pool->tags[mp_block_idx(pool, block)] = 0U` write inside
the task lock in `mempool_free`, after all correctness checks have passed.
`mempool_reset` already zeroed the entire tag array; the new per-block clear
keeps the invariant for individual frees.

---

### Bug 6.5 — `mempool_mgr_alloc` silently ignored non-OOM errors
**File:** `src/mempool_mgr.c`  
**Severity:** Medium (error masking)

The allocation loop in `mempool_mgr_alloc` continued to the next pool on
**any** error from `mempool_alloc`, including `MEMPOOL_ERR_NOT_INITIALIZED`
(which indicates that a pool's magic sentinel was corrupted — a serious
heap/stack corruption symptom).  This behaviour masked root-cause errors
with a generic "out of memory" response.

**Fix:** The loop now only continues to the next pool when the error is
`MEMPOOL_ERR_OUT_OF_MEMORY`.  All other errors are returned immediately,
preserving the diagnostic value of the error code.

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

## Formal Complexity Analysis

### mempool_alloc — O(1)

```
Operation                                     | Cost
----------------------------------------------|-------------------
NULL pointer checks                           | 2 comparisons
Magic sentinel check                          | 1 comparison
MEMPOOL_LOCK()                                | platform-defined, O(1)
ISR queue drain [if ISR_FREE=1]               | ≤ MEMPOOL_ISR_QUEUE_CAPACITY
  iterations, each:                           |
    dequeue under ISR lock                    |   1 load + 3 stores
    guard canary check [if GUARD=1]           |   1 load + 1 compare
    bitmap is_set + clear [if DFC=1]         |   2 loads + 1 store
    memset free-poison [if POISON=1]          |   O(block_size) — constant
    free-list prepend                         |   2 stores
    stats update                              |   ≤ 3 stores
free_list == NULL check (OOM)                 | 1 comparison
pop free-list head                            | 2 loads + 1 store
bitmap_set [if DFC=1]                         | 2 loads + 1 store
canary write [if GUARD=1]                     | 1 store
memset alloc-poison [if POISON=1]             | O(block_size) — constant
stats update [if STATS=1]                     | 4 stores
*block = nd; MEMPOOL_UNLOCK()                 | 1 store

No loop over pool blocks.  All costs are bounded by compile-time constants.
```
**Verdict: O(1). QED.**

Note for WCET analysis: when `MEMPOOL_ENABLE_ISR_FREE=1` AND
`MEMPOOL_ENABLE_POISON=1`, the worst-case path writes
`MEMPOOL_ISR_QUEUE_CAPACITY × block_size` bytes plus `block_size` bytes for
the alloc-poison fill.  All bounds are compile-time constants.

---

### mempool_free — O(1)

```
Operation                                     | Cost
----------------------------------------------|-------------------
NULL + magic checks                           | 3 comparisons
mp_validate_block (see below)                 | ≤ 6 ops
MEMPOOL_LOCK()                                | O(1)
canary read + compare [if GUARD=1]            | 2 ops
mp_bitmap_is_set + mp_bitmap_clear [if DFC=1]| ≤ 5 ops (1 memory load each)
tag clear [if TAGS=1]                         | 1 array store
memset free-poison [if POISON=1]              | O(block_size) — constant
free-list prepend                             | 2 stores
stats update [if STATS=1]                     | 3 stores
MEMPOOL_UNLOCK()                              | O(1)

No loop over pool blocks.
```
**Verdict: O(1) in terms of pool size N. QED.**

---

### mempool_is_block_allocated — O(1)

```
mp_validate_block(pool, block):
  Step 1 — range check:
    (uintptr_t)block < pool->blocks_start                 1 compare
    (uintptr_t)block >= pool->blocks_end                  1 compare
  Step 2 — offset:
    offset = (uintptr_t)block - (uintptr_t)blocks_start   1 sub
  Step 3 — alignment check:
    if block_shift != 0: offset & (stride-1) == 0         1 AND + 1 compare
    else:                offset % block_size == 0          1 mod + 1 compare

mp_block_idx(pool, block):
  if block_shift != 0: offset >> block_shift              1 right-shift
  else:                offset / block_size                1 division

mp_bitmap_is_set(pool, idx):
  bi = idx >> 3                                           1 right-shift
  m  = 1U << (idx & 7)                                   1 AND + 1 left-shift
  bi < pool->bitmap_bytes                                 1 compare
  return pool->bitmap[bi] & m                             1 load + 1 AND

Total: ≤ 13 scalar ops + 1 memory load.
No loops.  No traversal.  No recursion.
```
**Verdict: O(1). QED.**

---

### mempool_pool_buffer_size — O(1)

The function body is pure arithmetic: alignment-up (add + AND), multiplications,
and additions.  There are no loops, no function calls other than `mp_align_up`
and `mp_is_pow2` (both one-liners), and no memory accesses beyond the argument
values.
**Verdict: O(1). QED.**

---

### mempool_walk — O(N)

The loop `for (i = 0; i < pool->total_blocks; i++)` iterates once per block
regardless of how many are allocated.  The bitmap scan is `bitmap_bytes = ⌈N/8⌉`
bytes, each requiring one `mp_bitmap_is_set` call (O(1)).  Total: Θ(N).

---

### mempool_init, mempool_reset — O(N)

Both call `mp_build_free_list` (one loop over N blocks), and may call `memset`
over the bitmap (⌈N/8⌉ bytes) and tag array (4N bytes).  Total: Θ(N).

---

## Correctness Invariants

### Free-path ordering (critical)

Both `mempool_free()` and `mp_flush_isr_queue()` must follow this exact order:

1. **GUARD check** — validate post-canary _before_ touching any mutable state.
   - On failure: increment `guard_violations`, decrement `used_blocks`, **leave bitmap bit SET** (block is quarantined — a subsequent `mempool_free()` returns `DOUBLE_FREE`), return/skip.
2. **DOUBLE_FREE check** — inspect bitmap bit _before_ clearing it.
   - If bit is already 0: block is not allocated (double-free or duplicate ISR entry) → return `DOUBLE_FREE` / discard.
   - Otherwise: clear the bit.
3. **TAG clear** — zero `pool->tags[idx]` so the next owner sees a fresh tag.
4. **POISON fill** — `memset(FREE_POISON_BYTE)` over user area.
5. **Free-list prepend** — overwrites first `sizeof(void*)` bytes of the poison pattern (expected; documented).
6. **Stats update** — decrement `used_blocks`, increment `free_blocks`.

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

### Tag invariant (pass 6 addition)

After `mempool_free()` the tag for the freed block is always 0.  After
`mempool_init()` and `mempool_reset()` all tags are 0 (bitmap `memset`).
Invariant: a freshly allocated block always has `get_block_tag() == 0` unless
the caller explicitly sets a tag after allocation.

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
| DOUBLE_FREE_CHECK | +16 B | ⌈N/8⌉ bytes bitmap |
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
| ISR queue index truncation | `_Static_assert(MEMPOOL_ISR_QUEUE_CAPACITY <= 255)` | Compile error if misconfigured |
| State buffer too small | `_Static_assert(MEMPOOL_STATE_SIZE >= sizeof(struct mempool))` | Compile error if misconfigured |

### What mempool does NOT protect against

- **Arbitrary writes before `free()` is called** — the canary only catches writes that extend _past_ the user area into the canary word. Writes within the user area after free (use-after-free) are surfaced only by poison fills at the next access, not by the allocator itself.
- **Multi-word overruns that skip the canary** — a write that exceeds `block_size + 4` and lands in the next block is not detected (the next block's canary is intact).
- **Temporal double-free across pool resets** — after `mempool_reset()` the bitmap is cleared; a pointer held from before the reset will not be detected as a double-free.
- **Stack or global corruption leading to a corrupted pool state** — the magic sentinel provides one word of defence; a determined attacker that controls the sentinel can bypass all checks.
- **Thread-safety of the pool manager** — `mempool_mgr` does not take a lock itself; the underlying per-pool locks protect individual allocations but not the manager's iteration.

---

## Known Limitations

1. **ISR queue capacity is fixed at compile time** — `MEMPOOL_ISR_QUEUE_CAPACITY` must be chosen conservatively at build time (and must be ≤ 255). There is no runtime fallback if the queue fills faster than it drains.

2. **`mempool_walk()` holds the lock for the full duration** — the callback must not re-enter the pool. Long walk callbacks will block all other pool users for O(N) time.

3. **No cross-pool guard on `mempool_free()`** — if a caller passes a pointer from pool B to `mempool_free()` on pool A, and the pointer happens to fall within pool A's address range, the range check passes and the free succeeds silently (corrupting both pools). Users should use `mempool_contains()` as a pre-condition check in development builds.

4. **`mempool_mgr` is not thread-safe at the manager level** — concurrent `mempool_mgr_alloc()` calls on different threads can race on the iteration and routing logic. Protect the manager with an external lock if used from multiple threads.

5. **`mempool_alloc_tagged` uses two critical sections** — the allocation and the tag write are not combined into a single atomic operation. This is safe because the freshly allocated block is not visible to any other thread until this function returns and the caller publishes the pointer.

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
MEMPOOL_ISR_QUEUE_CAPACITY      /* ring buffer depth (default 8, max 255) */
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
| `mempool_gtest_hardening` | All features ON | ~422 |
| `mempool_gtest_mgr_nostats` | All features ON except STATS=0 | 6 |
| `mempool_ctest` | C test harness | 1 |
| **Total** | | **613** |

All tests pass on macOS (Apple Clang, `-fsanitize=address,undefined`) and the build is warning-free at `-Wall -Wextra -Wpedantic -Wconversion`.

