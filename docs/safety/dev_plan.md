# Mempool Library – Development & Verification Plan (Draft)

This document outlines the development and verification approach for the
mempool library using only free tools.

## 1. Development Practices

- Source control: Git
- Code review:
  - All non-trivial changes are peer-reviewed.
  - Reviews use checklists:
    - No new compiler warnings.
    - Coding style and guidelines followed.
    - Tests updated/added for new behavior.
    - API changes documented.

- Coding guidelines:
  - C11, no dynamic allocation, no recursion.
  - Avoid undefined behavior and implementation-defined constructs where
    practical.
  - Follow a MISRA-C-inspired subset; violations and justifications documented
    via static analysis reports and code reviews.

## 2. Static Analysis

- Compiler warnings:
  - Build with: `-Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wshadow -Wstrict-prototypes`
  - Zero warnings tolerated.

- Free static analysis tools:
  - `cppcheck --enable=all --std=c11 src/ tests/`
  - `clang-tidy` (where available) with a strict profile.

- All findings are triaged and either:
  - fixed, or
  - explicitly justified and documented.

## 3. Dynamic Analysis

- Sanitizers (Clang/GCC):
  - AddressSanitizer + UndefinedBehaviorSanitizer:
    - `-fsanitize=address,undefined -fno-omit-frame-pointer`
  - ThreadSanitizer for multithreaded tests:
    - `-fsanitize=thread -fno-omit-frame-pointer`

- All unit tests and GoogleTests are executed under these configurations where
  supported by the toolchain.

## 4. Testing Strategy

- Unit tests:
  - C test harness (`tests/test_mempool.c`).
  - GoogleTest suite (`tests/gtest_mempool.cpp`), including multi-threaded
    tests and stress-style patterns.

- Test execution:
  - Via CMake/CTest (`ctest`) and/or direct execution of binaries.
  - Automated in CI where possible.

## 5. Coverage Measurement

- On GCC/Clang-based platforms:
  - Compile with `--coverage -O0 -g` to enable gcov/lcov (or llvm-cov) tools.
  - Execute all tests.
  - Generate coverage reports (line/branch coverage) and archive them.

- Goal:
  - Near-100% line coverage of `src/mempool.c`.
  - High branch coverage; coverage reports are kept under `docs/coverage/` or
    attached to release artifacts.

## 6. Continuous Integration (CI)

- Use free CI services (e.g. GitHub Actions) to run:
  - Build + tests (Debug and possibly Release).
  - Static analysis (cppcheck, clang-tidy where available).
  - Optional sanitizer and coverage jobs.

- CI configuration is stored under `.github/workflows/` for transparency.

## 7. Release Criteria (Example)

A release is considered acceptable (e.g. `v1.0.0`) when:

- [ ] All unit tests and GoogleTests pass.
- [ ] No compiler warnings in supported configurations.
- [ ] Static analysis findings are either fixed or justified.
- [ ] Sanitizer runs show no issues on supported hosts.
- [ ] Coverage targets met (or deviations justified).
- [ ] Safety manual and requirements/traceability docs updated.
