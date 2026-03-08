# Changelog

## 0.5.2 (audit pass 8)

### Bug fixes
- **Canary misaligned access** (`src/mempool_core.c`, `src/mempool_isr.c`): The
  4-byte guard canary was written and read via a raw `uint32_t *` pointer cast at
  offset `user_block_size` from the block start.  When `block_size % 4 != 0` (e.g.
  9, 10, 11, 18, 21), this produced an unaligned 4-byte access — undefined behaviour
  in C and a hardware Hard Fault on ARM Cortex-M0/M0+ targets.  All 3 canary sites
  now use `memcpy` for both the write and the read.

- **`mempool_mgr_alloc` routed to undersized pool** (`src/mempool_mgr.c`): The
  routing loop compared the requested `min_size` against the physical stride
  (`mempool_block_size()`, which includes the guard canary and alignment padding)
  rather than the usable bytes per block.  A request for 33 bytes could be silently
  routed to a 32-byte-user-size pool (stride=40), providing only 32 usable bytes and
  causing a buffer overflow.  The comparison now uses the new
  `mempool_user_block_size()` accessor.  The insertion-sort in `mempool_mgr_init` was
  also fixed to sort by user block size rather than stride.

### New API
- `uint32_t mempool_user_block_size(const mempool_t *pool)` — O(1) accessor for the
  usable bytes per block (the `block_size` argument from `mempool_init`).  Always
  available regardless of feature flags.  Strictly less than `mempool_block_size()`
  when `MEMPOOL_ENABLE_GUARD=1`.

### Tests
- **+1667 tests** across two new test binaries (`mempool_gtest_stress`,
  `mempool_gtest_paranoia`).  Total: 2284 tests.
- `mempool_gtest_stress`: 113 pool configs × 10 stress patterns.  Includes block
  sizes with `% 4 != 0` (9, 10, 11, 18, 21) to specifically exercise the canary fix.
- `mempool_gtest_paranoia`: 30 configs × 16 deep-invariant tests + ~120 standalone
  security and regression tests (canary byte-by-byte tamper, ISR queue overflow,
  mgr routing correctness, null-pointer guards, OOM hook, etc.).

