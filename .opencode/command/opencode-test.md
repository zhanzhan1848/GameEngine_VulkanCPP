---
description: Build and test a feature worktree
---

Build and test the specified feature in its isolated worktree.

**Input**: The argument after `/opencode-test` is the feature name (kebab-case).

**Steps**

1. **Locate Worktree**
   Worktree path: `.worktrees/<name>`
   Verify this directory exists. If not, stop and inform user.

2. **Prepare Build Directory**
   Ensure `build/` directory exists inside the worktree.
   
   ```bash
   mkdir -p .worktrees/<name>/build
   ```

3. **Configure & Build**
   Run CMake in the worktree build directory.
   
   ```bash
   cd .worktrees/<name>/build
   cmake ..
   cmake --build .
   ```

4. **Run Tests**
   Run the tests (using ctest).
   
   ```bash
   cd .worktrees/<name>/build
   ctest --output-on-failure
   ```
   
   If tests fail, report the error to the user and stop.
   If tests pass, output "All tests passed!".
