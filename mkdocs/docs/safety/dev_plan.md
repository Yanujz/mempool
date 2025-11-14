# Development and Verification Plan (Free Tooling)

This document outlines a suggested development and verification flow for the
`mempool` library using only free tools. It is intended as a guideline and can
be adapted to local processes.

## Source Control and Reviews

- Maintain the code in a version-controlled repository (e.g. Git).
- Require code review for all non-trivial changes.
- Enforce that all changes run the full test and static analysis suite before
  being merged to the main branch.

---

## Coding Guidelines

- Use C11 with strict warnings enabled (e.g. `-Wall -Wextra -Wpedantic`).
- Avoid non-portable extensions in the core implementation.
- Prefer simple control flow and bounded loops.
- Check all return codes from library and system calls in tests and examples.

---

## Static Analysis

- Run `cppcheck` on C sources, tests, and examples.

Example command:

```bash
cppcheck   --suppress=missingIncludeSystem   --enable=all   --std=c11   --language=c   --inline-suppr   -Iinclude   src/ tests/*.c examples/*.c
```

- Optionally, use `clang-tidy` with a suitable configuration for additional
  diagnostics on host platforms.

---

## Dynamic Analysis (Sanitizers)

On host toolchains (GCC/Clang), build the library and tests with:

- AddressSanitizer (ASan)
- UndefinedBehaviorSanitizer (UBSan)
- ThreadSanitizer (TSAN) for multithreaded tests

CMake options are provided to enable these.

Recommended steps:

1. Configure a Debug build with sanitizers:

   ```bash
   cmake -B build -S . -DCMAKE_BUILD_TYPE=Debug          -DMEMPOOL_ENABLE_SANITIZERS=ON
   cmake --build build
   cd build && ctest --output-on-failure
   ```

2. For TSAN runs (mutually exclusive with ASan/UBSan):

   ```bash
   cmake -B build-tsan -S . -DCMAKE_BUILD_TYPE=Debug          -DMEMPOOL_ENABLE_SANITIZERS=OFF          -DMEMPOOL_ENABLE_TSAN=ON
   cmake --build build-tsan
   cd build-tsan && ctest --output-on-failure
   ```

---

## Code Coverage

- Build with coverage flags on host (through CMake or manual flags).
- Run the full test suite.
- Use `gcov`/`lcov` or `llvm-cov` to generate coverage reports.
- Aim to achieve high coverage on the core library and critical error paths.

Example (conceptual):

```bash
# Configure with coverage
cmake -B build-cov -S . -DCMAKE_BUILD_TYPE=Debug       -DMEMPOOL_ENABLE_COVERAGE=ON
cmake --build build-cov
cd build-cov && ctest

# Run coverage tools (commands depend on your tool choice)
```

---

## Continuous Integration

The provided CI configuration (GitHub Actions) demonstrates:

- Building with CMake.
- Running tests.
- Running `cppcheck` on the C source, tests, and examples.

Users can extend this with:

- Coverage upload (e.g. to Codecov),
- Additional static analysis steps,
- Platform-specific builds as needed.

---

## Release Checklist

For each tagged release, it is recommended to:

1. Run the full test suite (C harness + GoogleTest).
2. Run static analysis on the code base.
3. Run sanitizer-instrumented builds on host.
4. Generate and inspect coverage reports.
5. Document results and remaining open issues in the
   [Verification Report](verification_report_template.md).
6. Update the changelog with a summary of changes and their impact.

This document can be used as a basis for explaining the development approach
in larger safety cases or process documentation.
