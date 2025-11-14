# mempool – Deterministic Memory Pool Library

**mempool** is a small, portable memory pool library written in C11.

It is designed for embedded and safety-related systems that need:

- deterministic, fixed-size allocation and free,
- no use of `malloc` / `free`,
- explicit control over where state and buffers live,
- a simple API that works on bare metal and hosted targets.

The project uses:

- an **opaque** `mempool_t` state object stored in caller-provided memory,
- a caller-provided **pool buffer** that holds all allocation blocks,
- O(1) allocation and deallocation using a free list and bitmap,
- error codes for every failure path,
- optional hooks for integrating with a locking primitive on multi-threaded systems.

The repository is structured so it can fit into more formal processes (e.g. ISO 26262–style),
without requiring commercial tooling.

---

## Features

- Deterministic fixed-size block allocator
- Caller-owned state and pool memory (no dynamic allocation)
- Configurable block size and alignment
- Error codes (`mempool_error_t`) for all failure modes
- Statistics (`mempool_stats_t`) for usage monitoring
- Optional synchronization hooks for multi-threaded platforms
- C test harness + GoogleTest-based C++ tests (including multithreaded tests)
- CI with sanitizers and static analysis (cppcheck)

---

## Where to Start

- **[Quick Start](getting-started.md)** – minimal example and usage pattern
- **[Building](building.md)** – building on host, running tests, enabling sanitizers
- **[API Reference](api-core.md)** – complete description of the public API
- **[Safety & Process](requirements.md)** – requirements, traceability, and safety documentation

If you simply want to see it in action, check out the **Examples**:

- [Basic Usage](examples-basic.md)
- [Embedded](examples-embedded.md)
- [Stress & Testing](examples-stress.md)
