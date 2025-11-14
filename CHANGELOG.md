# Changelog

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [2.0.0] - 2025-11-14

### Added

- Per-block allocation bitmap for robust double-free detection
- `mempool_block_index` helper for constant-time block index calculation
- GoogleTest expectations for double-free and invalid pointer detection

### Changed

- Struct `mempool_t` fully initialized in `mempool_init` (no reliance on prior memory state)
- `mempool_reset` no longer re-calls `mempool_init`; instead rebuilds free-list
  and clears bitmap/stats in-place
- Thread-safety maintained via user-configured callbacks without changing API
- Project version bumped to 2.0.0 to reflect behavior/implementation changes

## [1.1.0] - 2025-11-14

### Added

- Optional synchronization callbacks (`mempool_set_sync`) for thread-safe use
- Internal locking helpers wrapping state mutations
- GoogleTest-based C++ test suite (`tests/gtest_mempool.cpp`)
- Multithreaded stress and correctness tests using `std::thread`/`std::mutex`
- CMake integration for fetching and building GoogleTest automatically

### Changed

- `mempool_get_stats` and `mempool_contains` use internal locking for consistency

## [1.0.0] - 2024-11-14

### Added

- Initial release of memory pool library
- Core API: init, alloc, free, get_stats, reset, contains
- Error handling with detailed error codes
- Statistics tracking (usage, peak, counts)
- O(1) allocation and deallocation performance
- Platform-agnostic implementation
- Basic test suite and examples
- Documentation (README, API reference)
- Build support (Makefile, CMake)
