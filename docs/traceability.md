# Mempool Library – Requirements Traceability

This document links requirements to implementation areas and tests.

| Req ID | Implementation                                    | Tests (non-exhaustive)                                         |
|--------|----------------------------------------------------|----------------------------------------------------------------|
| R-001  | `src/mempool.c` (no calls to malloc/free, etc.)   | Code review, greps, whole-file inspection                      |
| R-002  | Free-list + bitmap design in `src/mempool.c`      | All tests (timing assumed O(1); verified by code inspection)   |
| R-003  | `mempool_init` parameter checks                   | `InitSuccess`, `InitNullPointers`, `AlignmentAndSizeChecks`, `InitStateBufferTooSmall`, `PoolTooSmallForSingleBlock` |
| R-004  | Free-list exhaustion path in `mempool_alloc`      | `ExhaustiveAllocAndOutOfMemory`                               |
| R-005  | Bitmap checks in `mempool_free`                   | `DoubleFreeDetection`, `ResetResetsStatsAndFreeList`          |
| R-006  | Range and alignment checks in `mempool_free`      | `InvalidPointerDetection`                                     |
| R-007  | `mempool_stats_t` maintenance in ops              | `AllocFreeAndStats`, `PeakUsageTracking`, `ResetResetsStatsAndFreeList`, `ExhaustiveAllocAndOutOfMemory` |
| R-008  | `mempool_reset` implementation                    | `ResetResetsStatsAndFreeList`                                 |
| R-009  | `mempool_set_sync` + internal lock/unlock helpers | `ConcurrentAllocFree`, `ContainsUnderConcurrency`             |
| R-010  | `mempool_contains` implementation                 | `ContainsChecks`, `IndependentPools`, `ContainsUnderConcurrency` |
| R-011  | `mempool_strerror` implementation                 | `ErrorStringsNonNullAndNonEmpty`                              |
| R-012  | Opaque `mempool_t`, state size and macros         | `StateSizeWithinBound`, compile-time `_Static_assert` in `src/mempool.c` |

This table is intentionally minimal; individual project safety cases may add
more detailed links or additional requirement IDs.
