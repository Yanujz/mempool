# Traceability Overview

This document provides a high-level mapping between requirements, implementation
areas, and tests. It is not intended to be exhaustive, but to give a clear
starting point for a safety case.

## Legend

- **Req** – Requirement ID from [Requirements](requirements.md)
- **Implementation** – Files / functions where the requirement is primarily addressed
- **Tests** – C tests and GoogleTest cases that exercise the requirement

## Mapping Table

| Req   | Implementation                         | Tests                                                   |
|-------|----------------------------------------|---------------------------------------------------------|
| R-001 | `src/mempool.c` (no dynamic allocation)| C harness, all tests; GTest allocation tests           |
| R-002 | `mempool_alloc`, `mempool_free`       | `AllocFreeAndStats`, stress tests                      |
| R-003 | `mempool_init` block size config      | initialization tests, multi-pool tests                 |
| R-004 | `mempool_init` alignment checks       | alignment and misalignment tests                       |
| R-005 | `initialized` flag and checks         | tests calling APIs before initialization                |
| R-006 | all public functions return enum      | all tests (error paths asserted)                       |
| R-007 | bitmap tracking in `mempool_free`     | double-free detection tests                            |
| R-008 | range/alignment checks in `mempool_free` | invalid pointer tests                               |
| R-009 | `mempool_stats_t`, `mempool_get_stats`| stats and peak usage tests                             |
| R-010 | `mempool_reset`                       | reset behavior tests                                   |
| R-011 | `mempool_contains`                    | contains tests and multi-pool separation tests         |
| R-012 | `mempool_strerror`                    | error string tests                                     |
| R-013 | `mempool_set_sync`, internal lock hooks| multithreaded GTest cases                            |
| N-001 | use of C11, no OS deps                | inspection, host builds on GCC/Clang                   |
| N-002 | defensive checks / error codes        | error-path tests, static analysis                      |
| N-003 | separate tests / examples             | C harness, GoogleTest, examples                        |
| N-004 | stats, error strings, examples        | examples and API docs                                  |
| N-005 | `cppcheck` / sanitizers setup         | CI configuration, dev plan                             |

For more detail, see:

- [Safety Manual](safety/safety_manual.md)
- [Development Plan](safety/dev_plan.md)
- [Verification Report Template](safety/verification_report_template.md)
