# Verification Report Template

This document is a template for recording verification and validation results
for a specific release of the `mempool` library.

Fill in one copy per release and store it under version control.

---

## 1. Release Information

- **Component**: mempool
- **Version**: `vX.Y.Z`
- **Date**: `YYYY-MM-DD`
- **Commit ID**: `<git commit hash>`
- **Compiler / Toolchain**: `<e.g. GCC 13, Clang 17>`
- **Platforms Tested**: `<e.g. x86_64 Linux, ARM cross-compile>`


## 2. Summary

Brief summary of the verification activities for this release:

- Tests executed
- Static analysis runs
- Sanitizers / dynamic checks
- Coverage status
- Any deviations from the plan


## 3. Test Results

### 3.1 C Test Harness

- Binary: `mempool_ctest`
- Command: `./mempool_ctest`

| Result | Notes |
|--------|-------|
| Pass/Fail | Describe outcome and any notable observations |


### 3.2 GoogleTest Suite

- Binary: `mempool_gtest`
- Command: `./mempool_gtest`

| Result | Notes |
|--------|-------|
| Pass/Fail | Describe outcome and any notable observations |


### 3.3 Additional / Target-Specific Tests

Describe any hardware or target-specific tests performed:

- Test name:
- Platform:
- Procedure:
- Result:


## 4. Static Analysis

### 4.1 cppcheck

- Version: `<version>`
- Command:

```text
cppcheck --suppress=missingIncludeSystem --enable=all --std=c11 --language=c --inline-suppr -Iinclude src/ tests/*.c examples/*.c
```

- Result: `<Pass/Fail>`
- Notable findings and resolutions:


### 4.2 Other Tools (Optional)

E.g. `clang-tidy`, other static analyzers.

- Tool:
- Version:
- Configuration:
- Result:
- Notable findings:


## 5. Dynamic Analysis (Sanitizers)

Describe AddressSanitizer, UndefinedBehaviorSanitizer, and ThreadSanitizer runs,
if applicable.

- Build configuration:
- Commands:
- Result:
- Notable findings:


## 6. Code Coverage

Summarize coverage results:

- Tool:
- Commands:
- Overall coverage (%):
- Coverage on critical files:
- Notable gaps and justification:


## 7. Known Issues and Limitations

List any open issues, limitations, or deviations from requirements that are
known at the time of release.

- ID / reference:
- Description:
- Impact:
- Planned resolution:


## 8. Conclusion

Summarize whether the component is considered suitable for release under the
defined process, including any restrictions or additional steps required for
specific safety use cases.

- Overall assessment:
- Additional notes:


## 9. Approval (If Applicable)

- Prepared by: `Name, Role, Date`
- Reviewed by: `Name, Role, Date`
- Approved by: `Name, Role, Date`
