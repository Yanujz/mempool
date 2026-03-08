# mempool Security & Correctness Audit

**Library:** `mempool` — deterministic O(1) memory pool for embedded and safety-critical systems  
**Branch:** `feature/v2-optimized`  
**Current version:** 0.5.2  

---

## Audit History

| Pass | Scope | Findings | Outcome |
|------|-------|----------|---------|
| 1 (v0.4.0) | Gap analysis vs. production embedded requirements | 8 gaps: no guard canaries, no poison, no OOM hook, no ISR-safe free, no per-block tags, no pool manager, no alloc_zero, state size insufficient | Implemented all; 38 new hardening tests |
| 2 | Deep bug audit | 3 real bugs (compile failure when STATS=0, ISR drain never cleared bitmap bits, ISR free accepted unvalidated pointers) | 3 fixes; `mempool_block_size()` / `mempool_capacity()` added; 585→594 tests |
| 3 | Line-by-line invariant audit | 6 bugs (guard+poison missing on ISR drain, `mempool_reset()` ISR race, tag write outside lock, mgr accepts uninitialized handles, alloc_zero left `*block` non-NULL on error, `found_candidate` type) | 6 fixes; 16 new tests; 601 tests |
| 4 | Free-path ordering audit | 4 bugs (mempool_free bitmap cleared before guard check, ISR double-free undetected, alloc/mgr_alloc stale `*block` on error) | 4 fixes; 10 new tests; 611 tests |
| 5 | Code quality + ISR drain guard stats | 1 bug (ISR drain guard failure did not decrement `used_blocks`); refactored monolithic `mempool.c` into 3 focused translation units; added `mempool_pool_buffer_size()`, `mempool_walk()`, `mempool_is_block_allocated()` | 1 fix; split; new APIs; 611 tests |
| 6 | Cross-unit deduplication, type-safety, tag lifetime, error propagation, documentation | 5 bugs (see Pass 6 section); Doxyfile; README overhaul with porting guide + O(1) proofs | 5 fixes; 2 new tests; 613 tests |
| 7 | Tag lifetime, mgr state corruption, `mp_log2` performance, API completeness, examples | 4 bugs (see Pass 7 section); `mempool_has_free_block()`; `__builtin_ctz`; example fixes | 4 fixes; 5 new tests; 617 tests |
| 8 (this pass) | Canary alignment, mgr routing correctness, 2000+ stress tests | 3 bugs (see Pass 8 section); `mempool_user_block_size()` added; 2 new test suites | 3 fixes; +1667 tests; **2284 tests** |

---

## Pass 8 Bug Findings

### Bug 8.1 — Misaligned canary read/write when `block_size % 4 != 0`
**File:** `src/mempool_core.c`, `src/mempool_isr.c`  
**Severity:** High (C undefined behaviour; hardware fault on strict-alignment targets)

When `MEMPOOL_ENABLE_GUARD=1`, the 4-byte canary is written and read at address
`block_start + user_block_size`.  The original code performed this via a
`uint32_t *` pointer cast:

```c
uint32_t *canary = (uint32_t *)(void *)(block_start + user_block_size);
*canary = MEMPOOL_CANARY_VALUE;   /* write */
…
uint32_t cv = *canary;            /* read  */
```

When `user_block_size % 4 != 0` (e.g. block sizes 9, 10, 11, 18, 21…), the
canary address is not 4-byte aligned.  On ARM Cortex-M0/M0+ (and any target
with `UNALIGN_TRP` set), this raises a Hard Fault.  On x86 it is silent but
still undefined behaviour in C, making the code non-portable and flagged by
strict MISRA analysis.

All previous test configurations happened to use block sizes divisible by 4
(8, 12, 16, 20, 32, 48, 64, 128, 256), so this path was never exercised.

**Fix:** All 3 canary sites (2 in `mempool_core.c`, 1 in `mempool_isr.c`) now
use `memcpy` for the 4-byte read and write:

```c
uint32_t cv = MEMPOOL_CANARY_VALUE;
memcpy((uint8_t *)block_start + user_block_size, &cv, sizeof(cv));   /* write */
…
uint32_t cv;
memcpy(&cv, (const uint8_t *)block_start + user_block_size, sizeof(cv));  /* read */
```

`memcpy` is defined for any byte alignment; optimising compilers emit the same
single-register instruction as the pointer dereference when the alignment is
already 4, making this a zero-cost fix on modern toolchains.

---

### Bug 8.2 — `mempool_mgr_alloc` compared stride against `min_size`, not usable bytes
**File:** `src/mempool_mgr.c`  
**Severity:** High (silent buffer overflow — request could be routed to a pool
smaller than the requested size)

`mempool_mgr_alloc(mgr, min_size, …)` promises to return a block with at least
`min_size` usable bytes.  The routing loop used `mempool_block_size()` (which
returns the physical stride, including the 4-byte guard canary and alignment
padding) to gate pool selection:

```c
if ((size_t)mempool_block_size(mgr->pools[i]) < min_size) { continue; }
```

With `MEMPOOL_ENABLE_GUARD=1`, a 32-byte-user-size pool has a stride of
`align_up(32 + 4, 8) = 40`.  A request for `min_size = 33` would pass the
check (`40 >= 33`) and allocate from the 32-byte pool — providing only 32
usable bytes for a caller that requested 33, a classic buffer overflow.

The same stride-vs-user-size confusion affected `mgr_sort`, which sorted pools
by stride rather than by usable capacity, causing incorrect pool ordering when
guard padding moved a smaller pool's stride above a larger pool's user size.

**Fix:**
1. Added `uint32_t mempool_user_block_size(const mempool_t *pool)` to the
   public API — always available, returns the `block_size` argument from
   `mempool_init` (the caller-visible usable bytes).
2. Moved `user_block_size` out of the `#if MEMPOOL_ENABLE_GUARD` struct guard so
   it is always present in `struct mempool` regardless of feature flags.
3. `mgr_sort` now sorts by `mempool_user_block_size`.
4. The routing skip condition now reads:
   ```c
   if ((size_t)mempool_user_block_size(mgr->pools[i]) < min_size) { continue; }
   ```

---

### Bug 8.3 — `mempool_alloc` drains the ISR queue before allocating (test-level finding)
**File:** `src/mempool_core.c`  
**Severity:** Informational (correct behaviour, but latent documentation gap)

`mempool_alloc` unconditionally calls `mp_flush_isr_queue` when `isr_count > 0`,
before checking the free list.  This is correct (it recycles deferred frees to
satisfy the allocation), but it means that a test trying to verify ISR queue
overflow by alternating `mempool_free_from_isr` and `mempool_alloc` will
inadvertently drain the queue between the fill and the overflow check, making
the overflow unreachable.

This was not a code bug; it was a test design error caused by a missing
documentation note.  The fix was to restructure the `IsrQueue.FillToExactCapacity`
test to pre-allocate all required blocks (including the overflow probe) before
filling the ISR queue, avoiding any `mempool_alloc` call between fill and test.

**Documentation fix:** Added a comment in `mempool_core.c` at the ISR drain
site and updated this audit.

---

## Pass 8 Improvements

### Improvement 8.A — `mempool_user_block_size()` public API
**File:** `include/mempool.h`, `src/mempool_core.c`

New O(1) accessor that returns the number of usable bytes per block (the
`block_size` argument passed to `mempool_init`).  This is strictly less than
`mempool_block_size()` (stride) when `MEMPOOL_ENABLE_GUARD=1` (stride = user
size + 4 + alignment padding).  Always available regardless of feature flags.

Key use case: correctly sizing `min_size` arguments to `mempool_mgr_alloc`
without needing to subtract guard overhead manually.

---

### Improvement 8.B — 1667 new tests in two new test suites
**Files:** `tests/gtest_mempool_stress.cpp`, `tests/gtest_mempool_paranoia.cpp`

**`mempool_gtest_stress`** (1130 tests, 17 skipped due to small pool configs):  
- 113 pool configurations: block sizes 8–256 (including 9, 10, 11, 18, 21 with
  `% 4 != 0` to exercise the canary `memcpy` fix), 2–32 blocks, alignments 4
  and 8.
- 10 stress patterns per config: LIFO alloc-free, FIFO alloc-then-free, random
  free order, interleaved alloc-free, 5-cycle fill-drain, half-alloc-free,
  reset restores full capacity, stats consistency after every operation,
  alloc-poison pattern, `mempool_has_free_block` transitions.

**`mempool_gtest_paranoia`** (544 tests):  
30 base configs × 16 deep invariant test cases plus ~120 standalone tests covering:
- Canary tamper at each of the 4 canary bytes (byte-granular write verification)
- Double-free after realloc (LIFO: same block returned → first free succeeds,
  second is detected)
- Misaligned / out-of-range free pointer rejection
- Walk count vs. `stats.used_blocks` at full and half capacity
- ISR queue: fill to exact capacity, overflow detection, guard corruption in ISR
  drain path, FIFO ordering verification
- All 23 null-pointer guard entry points
- `mempool_init` validation: zero block size, non-power-of-2 alignment, too-small
  state buffer, too-small pool buffer
- `mempool_strerror` coverage of all 10 error codes
- Tag lifecycle: set/get on allocated block, rejection on freed block
- Peak tracking: peak survives reset when `reset_stats=false`
- Free-poison pattern inspection
- Mgr routing: 24 bytes → 32-byte pool; 33 bytes → 64-byte pool (verifies Bug 8.2 fix)
- Mgr corrupted-count guard
- OOM hook invocation counter
- Multi-pool isolation (cross-pool free returns `INVALID_BLOCK`)
- `mempool_user_block_size` == `mempool_block_size` − 4 when guard ON
- Version string non-empty

---

## Pass 7 Bug Findings

### Bug 7.1 — `mempool_set_block_tag` allowed writing tags to freed blocks
**File:** `src/mempool_diag.c`  
**Severity:** Medium (stale tag visible to next allocator)

When `MEMPOOL_ENABLE_TAGS=1` and `MEMPOOL_ENABLE_DOUBLE_FREE_CHECK=1`,
`mempool_set_block_tag` did not check whether the target block was currently
allocated.  A caller could write a tag to a freed block, and the pass 6 fix
that clears tags on `mempool_free` only cleared whatever was there at free
time — it could not defend against a tag written _after_ the free.  The next
allocator would see this tag as if it had been set by the previous owner.

**Fix:** Added a `mp_bitmap_is_set(pool, idx)` check inside the lock in
`mempool_set_block_tag`.  If the block is not allocated (bit = 0), the
function returns `MEMPOOL_ERR_INVALID_BLOCK` immediately.

---

### Bug 7.2 — `mempool_get_block_tag` allowed reading tags from freed blocks
**File:** `src/mempool_diag.c`  
**Severity:** Low (incorrect data returned, no corruption)

The symmetric counterpart of Bug 7.1: `mempool_get_block_tag` had no
allocation check.  Calling it on a freed block returned the tag that
`mempool_free` had just written (0) rather than signalling that the block
is not owned by anyone.  Callers using tags for ownership tracking could
receive misleadingly valid-looking data.

**Fix:** Same `mp_bitmap_is_set` check added inside the lock in
`mempool_get_block_tag`.

---

### Bug 7.3 — `mempool_mgr_alloc` / `mempool_mgr_free` unguarded `mgr->count`
**File:** `src/mempool_mgr.c`  
**Severity:** High (out-of-bounds array access under state corruption)

`mempool_mgr_init` validates that `count <= MEMPOOL_MGR_MAX_POOLS` at init
time.  However, if the `mempool_mgr_t` struct is later overwritten by a
stack or heap corruption, `mgr->count` could hold an arbitrarily large value.
The `for (i = 0; i < mgr->count; i++)` loops in `mempool_mgr_alloc` and
`mempool_mgr_free` would then walk past the `mgr->pools[]` array, causing
reads from (or writes to) arbitrary memory.

**Fix:** Added `if (mgr->count > (uint32_t)MEMPOOL_MGR_MAX_POOLS) return
MEMPOOL_ERR_INVALID_SIZE;` at the top of both functions, after the NULL
check.  This bounds the loop regardless of what a corrupted count field
contains.

---

### Bug 7.4 — Examples used undersized pool buffers
**File:** `examples/embedded_example.c`, `examples/basic_usage.c`,
`examples/stress_test.c`  
**Severity:** Low (documentation/example correctness)

`embedded_example.c` allocated `pool_buf[PACKET_SIZE * MAX_PACKETS]` (4096 B)
for 16 packets.  With all features enabled (guard canary + bitmap + tags),
each block's stride is 264 bytes and the overhead adds ~72 bytes, requiring
≥ 4296 bytes.  The 4096 B buffer would cause `mempool_init` to fit fewer than
the expected 16 blocks with no error message.  Additionally, the file had
`// #include <stdint.h>` as a dead comment, relying on the transitive include
through `mempool.h`.

**Fix:** All three example files now use `MEMPOOL_POOL_BUFFER_SIZE(block_size,
num_blocks, alignment)` for their pool buffers, which computes the correct
worst-case size regardless of which features are active.  The dead comment in
`embedded_example.c` was replaced with an active include.  The hardcoded `8U`
alignment literal was replaced with a named `POOL_ALIGN` constant.

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

### Tag invariant (pass 6 + pass 7 extension)

After `mempool_free()` the tag for the freed block is always 0.  After
`mempool_init()` and `mempool_reset()` all tags are 0 (bitmap `memset`).
Invariant: a freshly allocated block always has `get_block_tag() == 0` unless
the caller explicitly sets a tag after allocation.

**Pass 7 extension:** When `MEMPOOL_ENABLE_DOUBLE_FREE_CHECK=1`, both
`mempool_set_block_tag` and `mempool_get_block_tag` verify that the block's
bitmap bit is 1 (i.e., the block is currently allocated) before performing
any read or write.  This prevents a post-free tag write from corrupting the
invariant and prevents `get_block_tag` from returning stale data after a free.

### ISR queue invariant

`isr_count`, `isr_head`, and `isr_tail` are always mutated under `MEMPOOL_ISR_LOCK`. `mp_flush_isr_queue()` dequeues entries under `MEMPOOL_ISR_LOCK` one at a time; the rest of each entry's processing happens under the task-level `MEMPOOL_LOCK` only.

If `MEMPOOL_LOCK` and `MEMPOOL_ISR_LOCK` map to the same mutex, `mp_flush_isr_queue()` — called with `MEMPOOL_LOCK` held — **must not** attempt to acquire `MEMPOOL_ISR_LOCK` in a blocking way; this would deadlock. The test suite uses two separate mutexes to exercise both paths independently.

---

## Pass 7 Improvements

### Improvement 7.A — `mp_log2` uses `__builtin_ctz` on GCC/Clang
**File:** `src/mempool_internal.h`

The previous `mp_log2` implementation used a `while (v > 1)` loop of up to
32 iterations.  Although this function is called only during `mempool_init`
(not a hot path), replacing it with `__builtin_ctz(v)` — which maps to a
single `BSF`/`CTZ` instruction on x86 and ARM — removes a loop that would
show up as a WCET outlier in static analysis tools.  A plain C fallback is
retained for compilers without `__builtin_ctz`.

### Improvement 7.B — `mempool_has_free_block()` API
**File:** `include/mempool.h`, `src/mempool_diag.c`

Added `int mempool_has_free_block(const mempool_t *pool)`:

- **O(1)**: single pointer comparison `pool->free_list != NULL`.
- **Always available**: no feature flags required (unlike `mempool_get_stats`).
- **ISR-friendly**: no lock taken (free-list pointer read is atomic on all
  mainstream architectures when word-aligned).
- **Use case**: guard an allocation in a tight loop without the overhead of a
  full stats snapshot.

Complexity table update:

| Function | Complexity | Notes |
|----------|-----------|-------|
| `mempool_has_free_block` | **O(1)** | Single pointer load |

---

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
| Tag written to freed block | Bitmap check in `mempool_set_block_tag` (pass 7) | `MEMPOOL_ERR_INVALID_BLOCK` |
| Tag read from freed block | Bitmap check in `mempool_get_block_tag` (pass 7) | `MEMPOOL_ERR_INVALID_BLOCK` |
| Corrupted `mgr->count` field | Bounds check in `mempool_mgr_alloc/free` (pass 7) | `MEMPOOL_ERR_INVALID_SIZE` |
| Canary misaligned write/read (`block_size % 4 != 0`) | `memcpy`-based canary I/O (pass 8) | No UB; safe on strict-alignment targets |
| Mgr routing to pool smaller than request | `mempool_user_block_size()` comparison (pass 8) | Correct smallest-fitting-pool routing |

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
| `mempool_gtest` | STATS+DOUBLE_FREE+STRERROR; others OFF | 17 |
| `mempool_gtest_comprehensive` | Same as above | 515 |
| `mempool_gtest_hardening` | All features ON | 79 |
| `mempool_gtest_mgr_nostats` | All features ON except STATS=0 | 6 |
| `mempool_gtest_stress` | All features ON — 113 configs × 10 suites | 1123 |
| `mempool_gtest_paranoia` | All features ON — deep invariant + security | 544 |
| `mempool_ctest` | C test harness | 1 |
| **Total** | | **2284** |

Skipped tests: 17 in `mempool_gtest_stress` (single-block configs too small for
half-alloc-free pattern).

All tests pass on macOS (Apple Clang, `-Wall -Wextra -Wpedantic -Wconversion`,
sanitizers OFF for speed).  The canary misalignment fix (Bug 8.1) is specifically
exercised by the `ConfigsNPow` parameterisation in `gtest_mempool_stress.cpp`,
which includes block sizes 9, 10, 11, 18, and 21 — all with `% 4 != 0`.

