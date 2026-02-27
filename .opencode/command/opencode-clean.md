---
description: Remove a worktree and its feature branch
---

Clean up a worktree and delete the associated feature branch.

**Input**: The argument after `/opencode-clean` is the feature name (kebab-case).

**Steps**

1. **Verify Input**
   Ensure feature name is provided.

2. **Locate Worktree**
   Worktree path: `.worktrees/<name>`
   Branch name: `feature/<name>`
   
   If worktree directory does not exist, notify user and ask if they want to try deleting the branch anyway.

3. **Remove Worktree**
   Run:
   ```bash
   git worktree remove .worktrees/<name>
   ```
   
   If worktree is locked or has uncommitted changes, ask user confirmation before force removal.

4. **Delete Branch**
   Run:
   ```bash
   git branch -d feature/<name>
   ```
   
   If branch is not merged, use `-D` only if user confirms.

5. **Verify**
   Report "Cleanup complete for <name>".
