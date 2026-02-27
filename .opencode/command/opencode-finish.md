---
description: Test, merge, and clean a feature worktree
---

Automate the full integration process for a feature: build, test, merge, and clean up.

**Input**: The argument after `/opencode-finish` is the feature name (kebab-case).

**Steps**

1. **Verify Input**
   Ask user for feature name if not provided.

2. **Run Tests**
   Invoke `opencode-test` (or equivalent logic).
   
   If tests fail, abort immediately and notify user.

3. **Merge Feature**
   Invoke `opencode-merge` (or equivalent logic).
   
   If merge fails or has conflicts, abort and notify user.

4. **Clean Feature**
   Invoke `opencode-clean` (or equivalent logic).
   
   If clean fails, warn user but consider the process partially complete.

5. **Verify**
   Report "Feature <name> successfully merged and cleaned."
