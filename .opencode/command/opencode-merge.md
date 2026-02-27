---
description: Merge a completed feature into the main branch
---

Merge a feature branch into the main branch after tests pass.
**Input**: The argument after `/opencode-merge` is the feature name (kebab-case).

**Steps**

1. **Verify Input**
   Ensure feature name is provided.

2. **Locate Worktree**
   Ensure `.worktrees/<name>` exists.

3. **Check Branch Status**
   Branch: `feature/<name>`
   
   Run `git branch --show-current` to ensure we are in the main repository (e.g. `main` branch).
   
   If current branch is `feature/<name>`, warn user they should be on `main` to merge.

4. **Merge Feature**
   Run:
   ```bash
   git merge feature/<name>
   ```
   
   If merge conflicts occur, stop and notify user to resolve conflicts manually in the main repository.

5. **Verify**
   Report success and remind user to run `/opencode-clean <name>` to remove the worktree.
