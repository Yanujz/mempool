# Building and Running Tests

The repository supports both **CMake** and a traditional **Makefile**.

## Using CMake (Recommended for Host)

From the repository root:

```bash
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Debug
make
ctest --output-on-failure
```

### CMake Options

The project exposes several options to help with verification on host:

- `MEMPOOL_ENABLE_SANITIZERS` (ON/OFF)  
  Enables AddressSanitizer + UndefinedBehaviorSanitizer for GCC/Clang builds.

- `MEMPOOL_ENABLE_TSAN` (ON/OFF)  
  Enables ThreadSanitizer. This is mutually exclusive with `MEMPOOL_ENABLE_SANITIZERS`.

- `MEMPOOL_ENABLE_COVERAGE` (ON/OFF)  
  Adds coverage flags (e.g. `--coverage -O0`) so you can run `gcov`/`lcov` or `llvm-cov`.

Example:

```bash
cmake -B build -S . -DCMAKE_BUILD_TYPE=Debug       -DMEMPOOL_ENABLE_SANITIZERS=ON       -DMEMPOOL_ENABLE_COVERAGE=ON
cmake --build build -- -j"$(nproc)"
cd build && ctest --output-on-failure
```

This builds:

- `libmempool.a` – static library
- `mempool_ctest` – C test harness
- `mempool_gtest` – GoogleTest C++ test binary
- `example_basic`, `example_embedded`, `example_stress` – sample programs

---

## Using Make

For environments where CMake is not convenient, a simple Makefile is provided.

Typical usage:

```bash
make           # builds library + C test harness
make test      # runs the C test harness
make examples  # builds example binaries
make clean     # cleans build artefacts
```

Targets may vary slightly depending on your Makefile version; see the comments in `Makefile`.

---

## Running the Test Harness and Examples

After building with CMake:

```bash
cd build
./mempool_ctest       # C harness
./mempool_gtest       # GoogleTest-based tests

./example_basic       # basic usage demo
./example_embedded    # embedded-style packet buffer demo
./example_stress      # stress test demo
```

If sanitizers are enabled, they will report issues on the console if anything suspicious is detected.

---

## Static Analysis and Coverage

The repository is set up so CI can run:

- `cppcheck` on the C sources and examples
- Sanitizer-instrumented builds (Address, Undefined, and optional Thread)
- Coverage instrumentation via `--coverage`

For local runs:

```bash
# Example cppcheck invocation for C code
cppcheck   --suppress=missingIncludeSystem   --enable=all   --std=c11   --language=c   --inline-suppr   -Iinclude   src/ tests/*.c examples/*.c
```

See [Safety & Process → Development Plan](safety/dev_plan.md) for more detail
on recommended verification steps.
